#!/usr/bin/env python3
"""Whole-job (WJ) native SPU compiler -- stage 2 over the whole-block (WB)
backend (spu_wb_lifter.py). Design: docs/SPU_WB_ROADMAP.md stage 2.

What changes vs WB:

  * Blocks are connected into UNITS: weakly-connected components over the
    non-call control edges (fallthrough, direct branches, conditional
    branches, call-continuation). Calls (brsl/brasl/bisl) remain unit
    boundaries, so a unit is a whole SPU function (or a set of functions
    fused by tail branches).
  * Oversized units are split into address-contiguous COMPILE GROUPS
    (default cap 384 blocks); edges crossing a group seam publish and
    trampoline exactly like stage-1 cross-region hops.
  * Each group is ONE C function. Guest registers live in function-scoped
    __m128i locals (R3, R80, ...) that mirror the architectural registers
    at all times inside the group:
      - every ENTRY (entry switch case) runs a stub that loads every
        register the group touches, then jumps to its block;
      - intra-group control flow is plain `goto` carrying the locals --
        no ctx->gpr traffic on block boundaries or loop back-edges;
      - barriers (channel ops, calls, halts, stops, every transfer that
        can leave the group) PUBLISH the unpublished-dirty set to
        ctx->gpr, computed by a forward may-be-dirty dataflow (CARRY)
        over the group's goto edges -- publishing a clean register is a
        semantic no-op (locals always mirror), so the analysis only has
        to be conservative, never exact;
      - after a call the callee/drain may have modified any register, so
        the continuation jumps through its own entry stub (which is also
        the SPU_RET dispatch entry for that return site).
  * The registration table maps: group entry pcs -> the group function;
    every other pc -> the FAST region twin (the guarded fallback for
    unknown, patched, or unresolved behavior -- resumes, computed
    branches to arbitrary pcs, fallback blocks).

Everything else (kernels, LS handling, terminator semantics, the s41
pc-materialization invariant, per-block fallback classification) is
inherited from the WB lifter unchanged.

Usage mirrors spu_wb_lifter.py; batch driver: gen_spu_wb.py --lane wj.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from spu_disasm import disassemble_spu
from spu_lifter import (_ops, _reg, _imm, compute_bi_r0_jumps,
                        derive_region_prefix, SPULifter,
                        _NO_RT_WRITE, _RRR_DEST_OPS)
import spu_wb_lifter as WB
from spu_wb_lifter import (WBLifter, BlockEmitter, Block, IROp,
                           parse_fast_table, assert_pc_invariant,
                           VEC3_RRR, _COND_BR, _BRANCHY,
                           WB_HEADER_PREAMBLE, M32)


def _dest_of(insn):
    """Registers written by one instruction (guest reg indices)."""
    mn = insn.mnemonic
    if mn in ("rdch", "rchcnt", "fscrrd"):
        return [insn.raw & 0x7F]
    if mn in ("brsl", "brasl", "bisl", "bisled"):
        return [insn.raw & 0x7F]          # link register
    if mn in _NO_RT_WRITE:
        return []
    if mn in _RRR_DEST_OPS:
        return [(insn.raw >> 21) & 0x7F]
    return [insn.raw & 0x7F]


def _reads_of(insn):
    """Conservative guest register read set of one instruction (register
    fields that exist in its format; over-approximation is harmless -- it
    only widens the entry-stub load set)."""
    mn = insn.mnemonic
    if mn in ("nop", "lnop", "hbr", "hbra", "hbrr", "mtspr", "stop", "stopd",
              "sync", "dsync", "syncc", "il", "ila", "ilh", "ilhu", "fsmbi",
              "br", "bra", "brsl", "brasl", "iret", "fscrrd", "rdch",
              "rchcnt"):
        return []
    regs = {(insn.raw >> 7) & 0x7F, (insn.raw >> 14) & 0x7F}
    regs.add(insn.raw & 0x7F)            # rt is a source for many forms
    if mn in _RRR_DEST_OPS:
        regs.add((insn.raw >> 21) & 0x7F)
    return list(regs)


class Group:
    def __init__(self, gid, blocks):
        self.gid = gid
        self.blocks = blocks              # address-ordered compiled Blocks
        self.leaders = {b.leader for b in blocks}
        self.entries = set()              # cased pcs
        self.touched = set()              # guest regs read or written
        self.carry_in = {}                # leader -> frozenset(regs)
        self.name = None


class WJLifter(WBLifter):
    def __init__(self, *a, group_cap=384, **kw):
        super().__init__(*a, **kw)
        rp = derive_region_prefix(self.func_prefix)
        self.wj_prefix = "spu_wj" + rp[len("spu_region"):]
        self.group_cap = group_cap
        self.metrics.update({
            "wj_units": 0, "wj_groups": 0, "wj_entries": 0,
            "wj_interior_blocks": 0, "wj_entry_loads": 0,
            "wj_publish_stores": 0, "wj_intra_gotos": 0,
        })

    # -- CFG edges (non-call control flow between compiled blocks) --------
    def block_edges(self, blk):
        """Successor leader pcs reached WITHOUT leaving lifted execution:
        fallthrough, direct branch targets, call continuations."""
        succ = []
        insn = blk.insns[-1]
        mn = insn.mnemonic
        helper = SPULifter()
        tgt = helper._branch_target(insn)
        in_img = lambda t: self.image_span[0] <= t < self.image_span[1]
        if mn in ("br", "bra"):
            if tgt is not None and tgt != insn.addr and in_img(tgt):
                succ.append(tgt)
        elif mn in _COND_BR:
            if tgt is not None and in_img(tgt):
                succ.append(tgt)
            if blk.end in self.by_addr:
                succ.append(blk.end)
        elif mn in ("brsl", "brasl", "bisl", "bisled"):
            if insn.addr + 4 in self.by_addr:
                succ.append(insn.addr + 4)
            if mn in ("brsl", "brasl") and tgt == insn.addr + 4 \
                    and blk.end in self.by_addr:
                pass                        # pc-getter already covered
        elif mn in ("biz", "binz", "bihz", "bihnz"):
            if blk.end in self.by_addr:
                succ.append(blk.end)
        elif mn in ("bi", "iret", "stop", "stopd"):
            pass
        else:                               # fallthrough block
            if blk.end in self.by_addr:
                succ.append(blk.end)
        return [s for s in succ if s in self.block_owner]

    def build_units(self):
        # compiled blocks only; fallback blocks stay on the FAST twin
        compiled = {}
        for fn in self.funcs:
            for b in fn.blocks:
                if b.fallback is None:
                    compiled[b.leader] = b
        parent = {l: l for l in compiled}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        def union(a, b):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb

        for leader, blk in compiled.items():
            for s in self.block_edges(blk):
                if s in compiled:
                    union(leader, s)
        units = {}
        for leader in compiled:
            units.setdefault(find(leader), []).append(compiled[leader])
        self.metrics["wj_units"] = len(units)

        # image-wide entry evidence
        helper = SPULifter()
        call_targets = set()
        return_sites = set()
        for insn in self.insns:
            mn = insn.mnemonic
            if mn in ("brsl", "brasl"):
                t = helper._branch_target(insn)
                if t is not None:
                    call_targets.add(t)
            if mn in ("brsl", "brasl", "bisl", "bisled"):
                return_sites.add(insn.addr + 4)
        anchor_entries = call_targets | return_sites | \
            {self.image_span[0]}

        # split oversized units into contiguous groups + entry computation
        self.groups = []
        self.leader_group = {}
        gid = 0
        for _root, blocks in sorted(units.items(),
                                    key=lambda kv: min(b.leader for b in kv[1])):
            blocks.sort(key=lambda b: b.leader)
            chunks = [blocks[i:i + self.group_cap]
                      for i in range(0, len(blocks), self.group_cap)]
            for ch in chunks:
                g = Group(gid, ch)
                gid += 1
                self.groups.append(g)
                for b in ch:
                    self.leader_group[b.leader] = g
        # entries: anchors + cross-group edge targets
        for g in self.groups:
            for b in g.blocks:
                if b.leader in anchor_entries:
                    g.entries.add(b.leader)
                for s in self.block_edges(b):
                    tg = self.leader_group.get(s)
                    if tg is not None and tg is not g:
                        tg.entries.add(s)
        for g in self.groups:
            if not g.entries:                 # unreachable-from-anchors island:
                g.entries.add(g.blocks[0].leader)
            g.name = f"{self.wj_prefix}{min(g.entries):08X}"
        # guard: distinct names (two groups could share min-entry only if the
        # same pc were in both -- impossible by partition)
        assert len({g.name for g in self.groups}) == len(self.groups)
        self.metrics["wj_groups"] = len(self.groups)
        self.metrics["wj_entries"] = sum(len(g.entries) for g in self.groups)
        self.metrics["wj_interior_blocks"] = sum(
            len(g.blocks) - len(g.entries & g.leaders) for g in self.groups)

        # touched registers + block summaries + CARRY dataflow per group
        for g in self.groups:
            summaries = {}
            for b in g.blocks:
                writes = set()
                clears = False              # a mid/end publish clears carry
                for insn in b.insns:
                    for r in _dest_of(insn):
                        writes.add(r)
                    g.touched.update(_dest_of(insn))
                    g.touched.update(_reads_of(insn))
                    if insn.mnemonic in ("rdch", "wrch", "rchcnt"):
                        clears = True
                mn = b.insns[-1].mnemonic
                # publishing terminators clear carry on ALL outgoing paths
                # (cond-branch publish happens before the `if`; calls publish
                # then reload via entry stub)
                if mn in _BRANCHY or mn in ("hgt", "hlgt", "heq",
                                            "hgti", "hlgti", "heqi"):
                    term_publishes = mn not in ()      # see emitter: every
                    # terminator with a possibly-external edge publishes;
                    # pure intra-group branches do not. Resolve per edge:
                    term_publishes = self._term_publishes(b)
                else:
                    term_publishes = False
                summaries[b.leader] = (frozenset(writes), clears,
                                       term_publishes)
            # forward may-carry dataflow over intra-group goto edges
            carry = {b.leader: set() for b in g.blocks}
            changed = True
            while changed:
                changed = False
                for b in g.blocks:
                    writes, clears, term_pub = summaries[b.leader]
                    if clears or term_pub:
                        out = set(writes) if not term_pub else set()
                        if clears and not term_pub:
                            out = set(writes)   # conservative: writes after
                            # the clear are unknown statically -> all writes
                    else:
                        out = carry[b.leader] | writes
                    for s in self.block_edges(b):
                        if self.leader_group.get(s) is g:
                            # carry flows into ENTRY blocks too: an entry can
                            # be reached both via its stub (carry none -- the
                            # stub loads every touched register, so publishing
                            # a carried-but-clean register is a mirror store,
                            # a semantic no-op) and via intra-group gotos from
                            # predecessors with unpublished writes. Skipping
                            # entries here lost the fall-through carry into
                            # brsl-target prologue helpers (MEASURED: gs_task
                            # 0x3000 ila-ladder r2 unpublished at the 0x303C
                            # return -- the slice-1 task-image mismatches).
                            if not out <= carry[s]:
                                carry[s] |= out
                                changed = True
            g.carry_in = {l: frozenset(v) for l, v in carry.items()}

    def _term_publishes(self, blk):
        """True when the block's terminator publishes before its outgoing
        intra-group edges run (call/channel-class or any possibly-external
        edge). Mirrors the emitter's decision exactly."""
        insn = blk.insns[-1]
        mn = insn.mnemonic
        if mn not in _BRANCHY:
            return False                     # fallthrough: commit only
        if mn in ("brsl", "brasl", "bisl", "bisled", "bi", "iret",
                  "stop", "stopd"):
            return True
        helper = SPULifter()
        tgt = helper._branch_target(insn)
        in_group = lambda t: (t is not None and
                              self.leader_group.get(t) is
                              self.leader_group.get(blk.leader))
        if mn in ("br", "bra"):
            return not in_group(tgt)
        if mn in _COND_BR:
            ft_ok = in_group(blk.end) or blk.end not in self.by_addr
            return not (in_group(tgt) and ft_ok)
        if mn in ("biz", "binz", "bihz", "bihnz"):
            return True                      # conditional return/indirect
        return True

    # -- routing ----------------------------------------------------------
    def wj_symbol(self, pc):
        """Symbol executing in-image pc when entered with ctx->pc == pc, or
        None -> FAST/spu_indirect_branch."""
        g = self.leader_group.get(pc)
        if g is not None and pc in g.entries:
            return g.name
        return None


class WJBlockEmitter(BlockEmitter):
    """Stage-2 emitter: guest registers in function-scoped R locals."""

    def __init__(self, wl, group, blk):
        super().__init__(wl, wl.block_owner[blk.leader], blk)
        self.group = group
        self.carry = set(group.carry_in.get(blk.leader, ()))
        self.rlocal = {}                  # vid -> guest reg (R-local read)

    # locals mirror architecture: reads come from R locals
    def read_reg(self, r):
        r = int(r)
        if r in self.regmap:
            return self.regmap[r]
        v = self.newv()
        self.rlocal[v] = r
        self.regmap[r] = v
        self.reg_dirty[r] = False
        return v

    def vexpr(self, vid):
        if vid in self.rlocal:
            return f"R{self.rlocal[vid]}"
        return super().vexpr(vid)


    # flush: commit block-local writes to R locals; publish stores the
    # running carry (CARRY_in + writes since last publish) to ctx
    def flush_marker(self, publish=True):
        dirty = {r: v for r, v in self.regmap.items() if self.reg_dirty.get(r)}
        op = IROp(kind="flush")
        op.dirty = dict(dirty)
        op.publish = publish
        self.carry |= set(dirty)
        op.publish_set = frozenset(self.carry) if publish else frozenset()
        if publish:
            self.carry = set()
        self.ops.append(op)
        for r in dirty:
            self.reg_dirty[r] = False
        return op

    def translate(self, insn):
        # snapshot support for the conditional-halt in-path publish
        if insn.mnemonic in ("hgt", "hlgt", "heq", "hgti", "hlgti", "heqi"):
            before = dict(self.regmap)
            super().translate(insn)
            op = self.ops[-1]
            assert op.kind == "halt_cond"
            op.wj_publish = frozenset(
                self.carry | {r for r, v in self.regmap.items()
                              if self.reg_dirty.get(r)})
            return
        super().translate(insn)

    # -- emission overrides ----------------------------------------------
    def emit_ops(self):
        for op in self.ops:
            if not op.live:
                continue
            if op.kind == "flush":
                for r in sorted(op.dirty):
                    self.lines.append(f"    R{r} = {self.vexpr(op.dirty[r])};")
                    # the local now holds the committed value
                    vid = self.newv()
                    self.rlocal[vid] = r
                    self.regmap[r] = vid
                if op.publish:
                    for r in sorted(op.publish_set):
                        self.lines.append(
                            f"    WB_GPR_STORE(ctx, {r}, R{r});")
                        self.m["wj_publish_stores"] = \
                            self.m.get("wj_publish_stores", 0) + 1
                continue
            if op.kind == "halt_cond":
                self._emit_halt_cond(op)
                continue
            if op.kind == "gprload":
                # cannot happen (read_reg overridden) -- guard loudly
                raise AssertionError("gprload op in WJ emitter")
            self._emit_one(op)

    def _emit_one(self, op):
        # re-dispatch the single-op emission of the base class
        saved = self.ops
        self.ops = [op]
        try:
            BlockEmitter.emit_ops(self)
        finally:
            self.ops = saved

    def _emit_halt_cond(self, op):
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
        # dirty regs publish their translate-time value expression; carried
        # or already-committed regs publish the R local (always a mirror)
        flushes = "".join(
            f"WB_GPR_STORE(ctx, {r}, "
            f"{self.vexpr(op.dirty_snapshot[r]) if r in op.dirty_snapshot else f'R{r}'}); "
            for r in sorted(op.wj_publish))
        self.lines.append(
            f"    if ({cond}) {{ {flushes}ctx->pc = 0x{op.pc:X}u; "
            f"spu_halt(ctx, SPU_STATUS_STOPPED_BY_HALT); return; }}")

    # -- terminator routing ----------------------------------------------
    def prep_term(self, insn):
        publish = True
        if insn is None:
            publish = not self._in_group(self.blk.end)
        else:
            publish = self.wl._term_publishes(self.blk)
        # stage-1 prep_term reads cond/target regs and link defs, then
        # flushes; replicate with the chosen publish mode
        self.tail_roots = set()
        self.term = insn
        self.term_reads = {}
        if insn is not None:
            mn = insn.mnemonic
            ops = _ops(insn.operands)
            if mn in ("brsl", "brasl", "bisl", "bisled"):
                link_rt = insn.raw & 0x7F
                self.write_reg(link_rt,
                               self.add_const(((insn.addr + 4) & M32, 0, 0, 0)))
            if mn in _COND_BR or mn in ("biz", "binz", "bihz", "bihnz"):
                v = self.read_reg(_reg(ops[0]))
                self.term_reads["cond"] = v
                self.tail_roots.add(v)
            if mn in ("biz", "binz", "bihz", "bihnz"):
                tgt_reg = _reg(ops[1])
                if not (tgt_reg == "0" and insn.addr not in self.wl.bi_r0_jump):
                    tv = self.read_reg(tgt_reg)
                    self.term_reads["tgt"] = tv
                    self.tail_roots.add(tv)
            if insn.mnemonic == "bi":
                tgt_reg = _reg(ops[0])
                if not (tgt_reg == "0" and insn.addr not in self.wl.bi_r0_jump):
                    tv = self.read_reg(tgt_reg)
                    self.term_reads["tgt"] = tv
                    self.tail_roots.add(tv)
            if mn in ("bisl", "bisled"):
                tv = self.read_reg(_reg(ops[-1]))
                self.term_reads["tgt"] = tv
                self.tail_roots.add(tv)
        self.flush_marker(publish=publish)

    def _in_group(self, pc):
        return self.wl.leader_group.get(pc) is self.group and \
            pc in self.group.leaders

    def _goto_or_hop(self, tgt, debug_pc=None):
        wl = self.wl
        if self._in_group(tgt):
            wl.metrics["wj_intra_gotos"] += 1
            return f"goto b_{tgt:08X};"

        sym = wl.wj_symbol(tgt) or wl.fast_table.get(tgt) \
            or "spu_indirect_branch"
        wl.metrics["trampolines"] += 1
        dbg = (f"ctx->debug_indirect_source_pc = 0x{debug_pc:X}u; "
               if debug_pc is not None else "")
        return (f"{{ ctx->pc = 0x{tgt:X}u; {dbg}"
                f"g_spu_trampoline_fn = {sym}; return; }}")

    def emit_term(self):
        insn = self.term
        # calls: reroute the continuation through the entry stub; everything
        # else reuses the stage-1 emission against the overridden helpers
        if insn is not None and insn.mnemonic in ("brsl", "brasl"):
            helper = SPULifter()
            tgt = helper._branch_target(insn)
            addr = insn.addr
            L = self.lines
            wl = self.wl
            if tgt == addr:
                L.append(f"    ctx->pc = 0x{addr:X}u; "
                         f"ctx->status = SPU_STATUS_STOPPED_BY_HALT; return;")
                return
            if tgt == addr + 4:
                L.append(f"    {self._goto_or_hop(addr + 4)}")
                return
            in_image = wl.image_span[0] <= tgt < wl.image_span[1]
            callee = (wl.wj_symbol(tgt) or wl.fast_table.get(tgt)
                      or "spu_indirect_branch") if in_image \
                else "spu_indirect_branch"
            wl.metrics["calls"] += 1
            L.append(f"    ctx->pc = 0x{tgt:X}u; "
                     f"{{ int32_t _si = (int32_t)ctx->image_id; "
                     f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                     f"ctx->host_depth++; {callee}(ctx); SPU_DRAIN(ctx); "
                     f"ctx->host_depth--; spu_img_restore(ctx, _si); }}")
            if addr + 4 in wl.by_addr:
                if self._in_group(addr + 4):
                    L.append(f"    goto entry_{addr + 4:08X};")
                else:
                    L.append(f"    {self._goto_or_hop(addr + 4)}")
            return
        if insn is not None and insn.mnemonic == "bisl":
            s = self.sexpr(self.term_reads["tgt"])
            addr = insn.addr
            wl = self.wl
            wl.metrics["calls"] += 1
            self.lines.append(
                f"    {{ int32_t _si = (int32_t)ctx->image_id; "
                f"ctx->debug_indirect_source_pc = 0x{addr:X}u; "
                f"ctx->pc = {s}; ctx->host_depth++; "
                f"spu_indirect_branch(ctx); SPU_DRAIN(ctx); "
                f"ctx->host_depth--; spu_img_restore(ctx, _si); }}")
            if addr + 4 in wl.by_addr:
                if self._in_group(addr + 4):
                    self.lines.append(f"    goto entry_{addr + 4:08X};")
                else:
                    self.lines.append(f"    {self._goto_or_hop(addr + 4)}")
            return
        BlockEmitter.emit_term(self)


# ---------------------------------------------------------------------------
# Source emission
# ---------------------------------------------------------------------------

WJ_SOURCE_PREAMBLE = """\
/* Auto-generated by spu_wj_lifter.py (whole-job native SPU compiler) -- do
 * not edit by hand. Guest registers live in function-scoped locals; the
 * FAST region twin (always linked alongside) is the guarded fallback for
 * every non-entry pc. See docs/SPU_WB_ROADMAP.md stage 2. */
#include "{header_name}"
#include "spu_wb_simd.h"
"""


def emit_wj(wl: WJLifter, header_name):
    src = [WJ_SOURCE_PREAMBLE.replace("{header_name}", header_name), ""]
    hdr = [WB_HEADER_PREAMBLE]
    for s in sorted(set(wl.fast_table.values())):
        hdr.append(f"void {s}(spu_context* ctx);")
    hdr.append("")

    for g in wl.groups:
        hdr.append(f"void {g.name}(spu_context* ctx);")
    hdr.append("")
    hdr.append("void spu_register_function(uint32_t addr, void (*fn)(spu_context*));")
    hdr.append(f"void {wl.register_name}(void);")
    hdr.append(f"void {wl.register_name}_fastbase(void); /* renamed FAST registration */")
    hdr.append("")
    hdr.append("#ifdef __cplusplus")
    hdr.append("}")
    hdr.append("#endif")

    for g in wl.groups:
        src.append(f"void {g.name}(spu_context* ctx) {{")
        for r in sorted(g.touched):
            src.append(f"    __m128i R{r};")
        src.append("switch ((uint32_t)ctx->pc) {")
        for e in sorted(g.entries):
            src.append(f"case 0x{e:08X}u: goto entry_{e:08X};")
        src.append("default:")
        src.append("    /* pc preserved: entry switch default -- a non-entry"
                    " resume; the registry maps it to the FAST twin */")
        src.append("    g_spu_trampoline_fn = spu_indirect_branch;")
        src.append("    return;")
        src.append("}")
        for e in sorted(g.entries):
            src.append(f"entry_{e:08X}: ;")
            for r in sorted(g.touched):
                src.append(f"    R{r} = WB_GPR_LOAD(ctx, {r});")
                wl.metrics["wj_entry_loads"] += 1
            src.append(f"    goto b_{e:08X};")
        for blk in g.blocks:
            src.append(f"b_{blk.leader:08X}: {{")
            wrapped_before = wl.metrics["kernel_wrapped"]
            em = WJBlockEmitter(wl, g, blk)
            src.extend(em.translate_block())
            src.append("}")
            wl.n_insns_compiled += len(blk.insns)
            if wl.metrics["kernel_wrapped"] == wrapped_before:
                wl.n_blocks_helper_free += 1
        src.append("}")
        src.append("")

    # registration: entries -> group fn, everything else -> FAST
    src.append("typedef struct { uint32_t addr; void (*func)(spu_context*); } spu_wj_entry_t;")
    src.append("static const spu_wj_entry_t spu_wj_table[] = {")
    n_wj = 0
    for insn in wl.insns:
        pc = insn.addr
        sym = wl.wj_symbol(pc)
        if sym is not None:
            n_wj += 1
        else:
            sym = wl.fast_table.get(pc)
            if sym is None:
                continue
        src.append(f"    {{ 0x{pc:08X}u, {sym} }},")
    src.append("    { 0, 0 }")
    src.append("};")
    src.append("")
    src.append(f"void {wl.register_name}(void) {{")
    src.append("    if (!spu_wb_runtime_ok()) {")
    src.append(f'        fprintf(stderr, "[spu-wj] AVX2/OS support missing; '
               f'{wl.register_name} registering FAST twin only\\n");')
    src.append(f"        {wl.register_name}_fastbase();")
    src.append("        return;")
    src.append("    }")
    src.append("    for (const spu_wj_entry_t* e = spu_wj_table; e->func; ++e)")
    src.append("        spu_register_function(e->addr, e->func);")
    src.append("}")
    src.append("")
    return "\n".join(src), "\n".join(hdr), n_wj


def main():
    p = argparse.ArgumentParser(description="whole-job SPU compiler (WJ lane)")
    p.add_argument("input", nargs="?", default=None)
    p.add_argument("--auto-functions", metavar="ELF")
    p.add_argument("--base", type=lambda x: int(x, 0), default=0)
    p.add_argument("--func-prefix", required=True)
    p.add_argument("--register-name", required=True)
    p.add_argument("--fast-source", required=True)
    p.add_argument("--output", "-o", default=".")
    p.add_argument("--source-name", required=True)
    p.add_argument("--header-name", required=True)
    p.add_argument("--region-cap", type=int, default=256)
    p.add_argument("--group-cap", type=int, default=384)
    p.add_argument("--repair-pcs", default="")
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
    wl = WJLifter(insns, base, args.func_prefix, args.register_name,
                  fast_table, fast_register, region_cap=args.region_cap,
                  group_cap=args.group_cap)
    wl.bi_r0_jump = compute_bi_r0_jumps(insns, logical_bounds)
    wl.repair_pcs = {int(x, 0) for x in args.repair_pcs.split(",") if x}
    wl.build()
    wl.build_units()

    src, hdr, n_wj = emit_wj(wl, args.header_name)
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
    print(f"Wrote {sp}")
    print(f"Wrote {hp}")
    print(f"  WJ LIFT: {wl.metrics['wj_units']} unit(s) -> "
          f"{wl.metrics['wj_groups']} group fn(s), "
          f"{wl.metrics['wj_entries']} entr(ies), "
          f"{wl.metrics['wj_interior_blocks']} interior block(s) of "
          f"{n_blocks - n_fb} compiled ({n_fb} fallback), "
          f"{n_wj} entry pc(s) registered WJ")
    print(f"  PASS: pc-materialization invariant holds on every trampoline set")

    if args.metrics_json:
        m = dict(wl.metrics)
        m.update({
            "source": args.source_name,
            "register_name": args.register_name,
            "wj_prefix": wl.wj_prefix,
            "base": base,
            "image_span": list(wl.image_span),
            "n_insns": len(wl.insns),
            "n_blocks": n_blocks,
            "n_blocks_compiled": n_blocks - n_fb,
            "n_blocks_fallback": n_fb,
            "n_blocks_helper_free": wl.n_blocks_helper_free,
            "n_insns_compiled": wl.n_insns_compiled,
            "fallback_blocks": [[pc, r] for pc, r in wl.fallback_blocks],
            "wj_entry_pcs": n_wj,
            "unsupported": wl.unsupported,
            "unresolved_calls": wl.unresolved_calls,
            "source_bytes": len(src),
        })
        with open(args.metrics_json, "w") as f:
            json.dump(m, f, indent=1)
        print(f"  metrics -> {args.metrics_json}")


if __name__ == "__main__":
    main()
