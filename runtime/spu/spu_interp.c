/* SPU interpreter (2026-07-01 reassessment, item 3a) — SCAFFOLD.
 *
 * Purpose:
 *   1. in-process ORACLE: run an SPU image instruction-by-instruction and
 *      lockstep-diff against the lifted code, classifying every SPU anomaly
 *      as lift-bug vs runtime-bug on the spot (no instrumented-RPCS3 cycle);
 *   2. FALLBACK: first-contact execution of images we haven't lifted yet
 *      (Sofdec movie decoder = EBOOT SPU image #9 is ahead of us);
 *   3. in-process DISASSEMBLER (spu_interp_trace) for crash/halt paths —
 *      usable TODAY, no Python round-trip.
 *
 * The decoder (spu_interp_gen.h) is GENERATED from tools/spu_disasm.py by
 * tools/gen_spu_interp.py — the debugged Python tables + priority order stay
 * the single source of truth. Regenerate after any disasm table change.
 *
 * STATUS: decoder + trace are live; the semantic switch in spu_interp_step is
 * intentionally unimplemented (returns SPU_INTERP_UNIMPL). Semantics land
 * incrementally, verified per-op against CBEA + the tracediff harness
 * (tools/tracediff.py). Do NOT wire this into dispatch until then.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spu_interp_gen.h"

/* big-endian instruction fetch from LS */
static uint32_t spu_ifetch(const uint8_t* ls, uint32_t pc)
{
    pc &= 0x3FFFC;
    return ((uint32_t)ls[pc] << 24) | ((uint32_t)ls[pc + 1] << 16) |
           ((uint32_t)ls[pc + 2] << 8) | (uint32_t)ls[pc + 3];
}

/* ------------------------------------------------------------------ trace */

/* Format one decoded instruction into buf (in-process disassembler). */
void spu_interp_format(uint32_t insn, uint32_t pc, char* buf, size_t n)
{
    spu_insn d;
    spu_decode(insn, &d);
    const char* nm = spu_iop_name[d.op < SPU_IOP__COUNT ? d.op : 0];
    switch (d.fmt) {
    case SPU_FMT_RRR:
        snprintf(buf, n, "%-8s $r%u,$r%u,$r%u,$r%u", nm, d.rc, d.ra, d.rb, d.rt);
        break;
    case SPU_FMT_RI7:
        snprintf(buf, n, "%-8s $r%u,$r%u,%d", nm, d.rt, d.ra, d.i7);
        break;
    case SPU_FMT_RI8:
        snprintf(buf, n, "%-8s $r%u,$r%u,%u", nm, d.rt, d.ra, d.i8);
        break;
    case SPU_FMT_RI10:
        snprintf(buf, n, "%-8s $r%u,$r%u,%d", nm, d.rt, d.ra, d.i10);
        break;
    case SPU_FMT_RI16:
        snprintf(buf, n, "%-8s $r%u,0x%X (tgt 0x%05X)", nm, d.rt,
                 (uint32_t)d.i16 & 0xFFFF, (uint32_t)(pc + d.i16 * 4) & 0x3FFFC);
        break;
    case SPU_FMT_RI18:
        snprintf(buf, n, "%-8s $r%u,0x%X", nm, d.rt, d.i18);
        break;
    case SPU_FMT_CH:
        snprintf(buf, n, "%-8s $r%u,ch%u", nm, d.rt, d.ch);
        break;
    default:
        snprintf(buf, n, "%-8s $r%u,$r%u,$r%u", nm, d.rt, d.ra, d.rb);
        break;
    }
}

/* Print n instructions from LS starting at pc — for crash/halt diagnostics
 * (e.g. the "unknown branch pc=..." path in spu_channels.c). */
void spu_interp_trace(const uint8_t* ls, uint32_t pc, unsigned count)
{
    char buf[96];
    for (unsigned i = 0; i < count; ++i, pc = (pc + 4) & 0x3FFFC) {
        uint32_t insn = spu_ifetch(ls, pc);
        spu_interp_format(insn, pc, buf, sizeof buf);
        fprintf(stderr, "  [spu-disasm] %05X: %08X  %s\n", pc, insn, buf);
    }
    fflush(stderr);
}

/* ------------------------------------------------------------- step frame */

enum spu_interp_status {
    SPU_INTERP_OK = 0,        /* executed, pc advanced */
    SPU_INTERP_BRANCH,        /* executed, pc set by the instruction */
    SPU_INTERP_STOP,          /* stop/halt reached */
    SPU_INTERP_CHANNEL,       /* needs a channel op (caller routes to spu_channels) */
    SPU_INTERP_UNIMPL,        /* semantic not implemented yet (scaffold) */
    SPU_INTERP_BADOP,         /* undecodable word */
};

/* One instruction. regs = 128 x 16-byte SPU registers, SPU word 0 in _u32[0]
 * (this repo's layout — see spu_context.h / the spu_link proof 2026-07-01).
 * Returns spu_interp_status; *pc is updated on OK/BRANCH. */
int spu_interp_step(uint8_t* ls, void* regs, uint32_t* pc)
{
    (void)regs;
    uint32_t insn = spu_ifetch(ls, *pc);
    spu_insn d;
    spu_decode(insn, &d);
    if (d.op == SPU_IOP_UNKNOWN)
        return SPU_INTERP_BADOP;

    switch (d.op) {
    /* Semantics land here incrementally: start with the
     * loads/stores + integer ALU the tracediff harness exercises, verify each
     * op in lockstep against the lifted gs_task run before adding the next
     * tranche. CBEA is the authority; RPCS3 is a read-only cross-check. */
    default: {
        static int warned[SPU_IOP__COUNT];
        if (d.op < SPU_IOP__COUNT && !warned[d.op]) {
            warned[d.op] = 1;
            fprintf(stderr, "[spu-interp] unimplemented op '%s' @0x%05X (scaffold)\n",
                    spu_iop_name[d.op], *pc);
        }
        return SPU_INTERP_UNIMPL;
    }
    }
}

/* Decoder self-test: known encodings decode to the right op (callable from a
 * diag path; returns 0 on pass). Vectors cross-checked vs tools/spu_disasm.py. */
int spu_interp_selftest(void)
{
    static const struct { uint32_t insn; const char* op; } v[] = {
        { 0x24004080u, "stqd" },     /* stqd from real gs_task code */
        { 0x1CE00080u, "ai"   },
        { 0x33800280u, "lqr"  },     /* lqr (RI16 0x067) */
        { 0x33000280u, "brsl" },     /* brsl (RI16 0x066) */
        { 0x35000000u, "bi"   },     /* bi $r0 */
        { 0x00000000u, "stop" },
        { 0x40800003u, "il"   },
    };
    int fail = 0;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; ++i) {
        spu_insn d;
        spu_decode(v[i].insn, &d);
        const char* got = spu_iop_name[d.op < SPU_IOP__COUNT ? d.op : 0];
        if (strcmp(got, v[i].op) != 0) {
            fprintf(stderr, "[spu-interp] SELFTEST FAIL %08X: got %s want %s\n",
                    v[i].insn, got, v[i].op);
            fail = 1;
        }
    }
    return fail;
}
