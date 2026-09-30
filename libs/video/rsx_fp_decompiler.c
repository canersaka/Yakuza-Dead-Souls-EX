/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) -> HLSL decompiler
 *
 * See rsx_fp_decompiler.h.
 *
 * Encoding: NV40 fragment program, four words (16 bytes) per instruction; a
 * CONST source is followed by a 16-byte inline constant slot. The layout
 * follows the publicly documented NV40 nvfx encoding. Bit positions are
 * spelled out as named macros below.
 */

#include "rsx_fp_decompiler.h"
#include "rsx_hash.h"
#include <stdio.h>
#include <string.h>

/* ---- DWORD 0 (OPDEST) --------------------------------------------------- */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9          /* X@9 Y@10 Z@11 W@12 */
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)

/* ---- DWORD 1/2/3 (SRC0/1/2) --------------------------------------------- */
#define FP_REG_TYPE_SHIFT   0
#define FP_REG_TYPE_MASK    (3u << 0)
#define   FP_REG_TYPE_TEMP  0
#define   FP_REG_TYPE_INPUT 1
#define   FP_REG_TYPE_CONST 2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9          /* X@9 Y@11 Z@13 W@15, 2 bits each */
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29) /* in DWORD1 */
#define FP_SRC1_ABS         (1u << 18) /* in DWORD2 */
#define FP_OPCODE_HI        (1u << 31) /* DWORD2 bit31: opcode bit 6 */

/* ---- Execution-condition / condition-code fields (SRC0 = DWORD1) ---------
 * The NV40 fragment ISA predicates each instruction on a condition register
 * (CC0/CC1). SRC0 layout: exec_if_lt@18, exec_if_eq@19, exec_if_gr@20,
 * cond_swizzle@21..28, cond_mod_reg_index@30, cond_reg_index@31; OPDEST.set_cond
 * is bit 8. All three exec bits set (0b111) == unconditional (the default);
 * none set == never. Programs may pair a condition-code write with later
 * predicated instructions (including conditional TEX). */
#define FP_EXEC_IF_LT       (1u << 18)
#define FP_EXEC_IF_EQ       (1u << 19)
#define FP_EXEC_IF_GR       (1u << 20)
#define FP_COND_SWZ_SHIFT   21          /* X@21 Y@23 Z@25 W@27, 2 bits each */
#define FP_COND_MOD_REG     (1u << 30)  /* set_cond WRITE target: cc0/cc1     */
#define FP_COND_REG         (1u << 31)  /* exec_if READ source:   cc0/cc1     */

/* ---- Opcodes ------------------------------------------------------------ */
enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_MUL=0x02, OP_ADD=0x03, OP_MAD=0x04,
    OP_DP3=0x05, OP_DP4=0x06, OP_DST=0x07, OP_MIN=0x08, OP_MAX=0x09,
    OP_SLT=0x0A, OP_SGE=0x0B, OP_SLE=0x0C, OP_SGT=0x0D, OP_SNE=0x0E,
    OP_SEQ=0x0F, OP_FRC=0x10, OP_FLR=0x11, OP_KIL=0x12, OP_PK4B=0x13,
    OP_UP4B=0x14, OP_DDX=0x15, OP_DDY=0x16, OP_TEX=0x17, OP_TXP=0x18,
    OP_TXD=0x19, OP_RCP=0x1A, OP_RSQ=0x1B, OP_EX2=0x1C, OP_LG2=0x1D,
    OP_LIT=0x1E, OP_LRP=0x1F, OP_STR=0x20, OP_SFL=0x21, OP_COS=0x22,
    OP_SIN=0x23, OP_PK2H=0x24, OP_UP2H=0x25, OP_POW=0x26, OP_PK4UB=0x27,
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_DP2A=0x2E, OP_TXL=0x2F,
    OP_TXB=0x31, OP_RFL=0x36, OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
    OP_DIVSQ=0x3B, OP_LIF=0x3C, OP_FENCT=0x3D, OP_FENCB=0x3E
};

#define OP_RET 0x45u
#define OP_IFE 0x42u
#define OP_LOOP 0x43u
typedef struct fp_flow_scope {u32 end,alternative;} fp_flow_scope;

static u32 fp_full_opcode(u32 w0, u32 w2)
{
    return ((w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT) |
           ((w2 & FP_OPCODE_HI) ? 0x40u : 0u);
}

static int fp_opcode_is_flow(u32 opcode)
{
    return opcode >= 0x40u;
}

const char* rsx_fp_opcode_name(u32 op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV"; case OP_MUL: return "MUL";
    case OP_ADD: return "ADD"; case OP_MAD: return "MAD"; case OP_DP3: return "DP3";
    case OP_DP4: return "DP4"; case OP_DST: return "DST"; case OP_MIN: return "MIN";
    case OP_MAX: return "MAX"; case OP_SLT: return "SLT"; case OP_SGE: return "SGE";
    case OP_SLE: return "SLE"; case OP_SGT: return "SGT"; case OP_SNE: return "SNE";
    case OP_SEQ: return "SEQ"; case OP_FRC: return "FRC"; case OP_FLR: return "FLR";
    case OP_KIL: return "KIL"; case OP_DDX: return "DDX"; case OP_DDY: return "DDY";
    case OP_TEX: return "TEX"; case OP_TXP: return "TXP"; case OP_TXD: return "TXD";
    case OP_RCP: return "RCP"; case OP_RSQ: return "RSQ"; case OP_EX2: return "EX2";
    case OP_LG2: return "LG2"; case OP_LIT: return "LIT"; case OP_LRP: return "LRP";
    case OP_STR: return "STR"; case OP_SFL: return "SFL"; case OP_COS: return "COS";
    case OP_SIN: return "SIN"; case OP_POW: return "POW"; case OP_DP2A: return "DP2A";
    case OP_TXL: return "TXL"; case OP_TXB: return "TXB"; case OP_RFL: return "RFL";
    case OP_DIV: return "DIV"; case OP_DP2: return "DP2"; case OP_NRM: return "NRM";
    case OP_DIVSQ: return "DIVSQ"; case OP_LIF: return "LIF";
    case OP_FENCT: return "FENCT"; case OP_FENCB: return "FENCB";
    default:     return "?";
    }
}

/* ------------------------------------------------------------------------- */

u32 rsx_fp_read_word(const u8* p)
{
    u32 be = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (be << 16) | (be >> 16);   /* 16-bit half-word swap */
}

u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        /* A CONST source pulls an inline 16-byte constant slot. */
        if (!fp_opcode_is_flow(fp_full_opcode(w0, w2)) &&
            (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
             ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
             ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST)) {
            if (off + 16 <= max_bytes) off += 16;
        }
        if (w0 & FP_END) return off;
    }
    return 0;
}

int rsx_fp_validate_control_flow(const u8* code,u32 max_bytes)
{
    const u32 size=rsx_fp_program_size(code,max_bytes);
    if(!size || size>65536 || (size&15))return 0;
    u8 boundary[4097]={0};fp_flow_scope stack[32];u32 depth=0;
    for(u32 at=0;at<size;) {
        boundary[at/16]=1;
        const u32 w0=rsx_fp_read_word(code+at),w1=rsx_fp_read_word(code+at+4);
        const u32 w2=rsx_fp_read_word(code+at+8),w3=rsx_fp_read_word(code+at+12);
        at+=16;
        if(!fp_opcode_is_flow(fp_full_opcode(w0,w2)) && ((w1&3)==2 || (w2&3)==2 || (w3&3)==2))at+=16;
        if(at>size)return 0;
    }
    boundary[size/16]=1;
    for(u32 at=0;at<size;) {
        while(depth && stack[depth-1].end==at)--depth;
        if(depth && stack[depth-1].alternative==at)stack[depth-1].alternative=0;
        const u32 w0=rsx_fp_read_word(code+at),w1=rsx_fp_read_word(code+at+4);
        const u32 w2=rsx_fp_read_word(code+at+8),w3=rsx_fp_read_word(code+at+12);
        const u32 opcode=fp_full_opcode(w0,w2);at+=16;
        if((w0&FP_END) && depth)return 0; // Mid-scope termination needs its own contract.
        if(opcode==OP_IFE || opcode==OP_LOOP) {
            if(depth==32 || (w0&FP_END) || w3>size/4)return 0;
            const u32 end=w3*4,alternative=opcode==OP_IFE?(w2&0x7fffffffu)*4u:end;
            if(end<at || (end&15) || !boundary[end/16] ||
               (opcode==OP_IFE && (w2&0x7fffffffu)>size/4) ||
               alternative<at || alternative>end || (alternative&15) || !boundary[alternative/16])return 0;
            if(depth && end>(stack[depth-1].alternative?stack[depth-1].alternative:stack[depth-1].end))return 0;
            if(opcode==OP_LOOP && !((w2>>19)&255) && ((w2>>10)&255)<((w2>>2)&255))return 0;
            stack[depth++]=(fp_flow_scope){end,alternative<end?alternative:0};
        } else if(fp_opcode_is_flow(opcode)) {
            if(opcode!=OP_RET)return 0;
        } else {
            // Indexed inputs require the hardware loop/address register. Do
            // not silently treat that distinct feature as a fixed input.
            if(w3&(1u<<30))return 0;
            if((w1&3)==2 || (w2&3)==2 || (w3&3)==2)at+=16;
        }
    }
    while(depth && stack[depth-1].end==size)--depth;
    return !depth;
}

static int fp_instruction_has_constant(u32 w1, u32 w2, u32 w3)
{
    return
        ((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST;
}

int rsx_fp_plan_constants(const u8* ucode,u32 max_bytes,rsx_fp_constant_layout* out)
{
    if(!ucode||!out)return -1;
    out->bytes=out->count=0;
    u32 off=0;
    while(off+16u<=max_bytes){
        const u32 w0=rsx_fp_read_word(ucode+off),w1=rsx_fp_read_word(ucode+off+4);
        const u32 w2=rsx_fp_read_word(ucode+off+8),w3=rsx_fp_read_word(ucode+off+12);
        off+=16;
        if(!fp_opcode_is_flow(fp_full_opcode(w0,w2))&&fp_instruction_has_constant(w1,w2,w3)){
            if(off+16u>max_bytes||out->count>=RSX_FP_MAX_INLINE_CONSTANTS)return -1;
            out->offsets[out->count++]=off;off+=16;
        }
        if(w0&FP_END){out->bytes=off;return (int)out->count;}
    }
    return -1;
}
int rsx_fp_structure_matches(const u8* source,const u8* snapshot,u32 bytes,
    const rsx_fp_constant_layout* layout)
{
    if(!source||!snapshot||!layout||!layout->bytes||bytes<layout->bytes||
        layout->count>RSX_FP_MAX_INLINE_CONSTANTS)return 0;
    u32 cursor=0;
    for(u32 i=0;i<layout->count;i++){
        const u32 off=layout->offsets[i];
        if(off<cursor||off>layout->bytes||layout->bytes-off<16u)return 0;
        if(memcmp(source+cursor,snapshot+cursor,off-cursor))return 0;
        cursor=off+16;
    }
    return !memcmp(source+cursor,snapshot+cursor,layout->bytes-cursor);
}
int rsx_fp_refresh_constants(const u8* ucode,u32 bytes,
    const rsx_fp_constant_layout* layout,rsx_fp_constant_block* out)
{
    if(!ucode||!layout||!out||!layout->bytes||bytes<layout->bytes||
        layout->count>RSX_FP_MAX_INLINE_CONSTANTS)return -1;
    for(u32 i=0;i<layout->count;i++)
        if(layout->offsets[i]>layout->bytes||layout->bytes-layout->offsets[i]<16u)return -1;
    out->count=layout->count;
    /* Only count slots are observable/uploaded. Do not clear the maximum
     * 32 KiB capacity for every draw with a handful of inline constants. */
    for(u32 i=0;i<layout->count;i++)for(u32 lane=0;lane<4;lane++)
        out->values[i][lane]=rsx_fp_read_word(ucode+layout->offsets[i]+lane*4);
    return (int)out->count;
}

int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    if (!ucode || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (!fp_opcode_is_flow(fp_full_opcode(w0, w2)) &&
            fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_read_word(ucode + off + component * 4);
            out->count++;
            off += 16;
        }
        if (w0 & FP_END)
            return (int)out->count;
    }
    return -1;
}

u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed)
{
    if (!ucode)
        return 0;
    static const u32 structural_tag = 0x31535046u; /* "FPS1" */
    u64 hash = rsx_fnv1a(&structural_tag, sizeof(structural_tag), seed);
    u32 off = 0;
    u32 instruction_count = 0;
    u32 constant_count = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        hash = rsx_fnv1a(ucode + off, 16, hash);
        instruction_count++;
        off += 16;
        if (!fp_opcode_is_flow(fp_full_opcode(w0, w2)) &&
            fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                constant_count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return 0;
            constant_count++;
            off += 16;
        }
        if (w0 & FP_END) {
            hash = rsx_fnv1a(
                &instruction_count, sizeof(instruction_count), hash);
            hash = rsx_fnv1a(
                &constant_count, sizeof(constant_count), hash);
            return hash;
        }
    }
    return 0;
}

int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes)
{
    if (!out_offset || !out_allocation_bytes || data_bytes == 0)
        return -1;
    if (data_bytes > 0xFFFFFF00u)
        return -1;
    const u32 allocation = (data_bytes + 255u) & ~255u;
    if (allocation > capacity)
        return -1;
    if (used > capacity || allocation > capacity - used)
        return 0;
    *out_offset = used;
    *out_allocation_bytes = allocation;
    return 1;
}

int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset)
{
    if (!out_offset || block_bytes == 0 || (block_bytes & 255u) != 0 ||
        block_bytes > capacity)
        return -1;
    if (used > capacity || block_bytes > capacity - used)
        return 0;
    *out_offset = used;
    return 1;
}

/* Bounded string appender. */
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;

static void out_puts(Out* o, const char* s)
{
    if (!o->ok) return;
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

/* Decoded view of one source word. */
typedef struct {
    u32 type;       /* FP_REG_TYPE_* */
    u32 index;
    int half;
    int negate;
    int abs;
    char swz[5];    /* e.g. "xyzw" */
} Src;

static void decode_src(u32 w, int abs_bit, Src* s)
{
    static const char comp[4] = {'x','y','z','w'};
    s->type   = (w & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT;
    s->index  = (w & FP_SRC_REG_MASK)  >> FP_SRC_REG_SHIFT;
    s->half   = (w & FP_SRC_HALF) ? 1 : 0;
    s->negate = (w & FP_SRC_NEGATE) ? 1 : 0;
    s->abs    = abs_bit ? 1 : 0;
    for (int i = 0; i < 4; i++)
        s->swz[i] = comp[(w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3];
    s->swz[4] = '\0';
}

/* HLSL expression for the interpolated input selected by OPDEST.INPUT_SRC.
 * The backend's passthrough VS forwards COL0/COL1/FOG/TEXCOORD0..7; WPOS maps
 * to SV_POSITION (screen-space, an approximation of the RSX window coord). */
static const char* input_expr(u32 input_src)
{
    static const char* tc[8] = {
        "input.tc0", "input.tc1", "input.tc2", "input.tc3",
        "input.tc4", "input.tc5", "input.tc6", "input.tc7" };
    switch (input_src) {
    case 0x0: return "input.position"; /* WPOS */
    case 0x1: return "input.col0";     /* COL0 */
    case 0x2: return "input.col1";     /* COL1 */
    case 0x3: return "input.fog";      /* FOGC */
    default:
        if (input_src >= 0x4 && input_src <= 0xB) return tc[input_src - 0x4]; /* TC0..7 */
        return "float4(0,0,0,0)";      /* TC8/TC9/FACING not plumbed */
    }
}

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`. */
static void emit_src(const Src* s, u32 input_src, const float* k, int has_k,
                     int buffered, u32 constant_slot, char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        snprintf(base, sizeof(base), "%s", input_expr(input_src));
    } else { /* CONST */
        if (buffered && has_k)
            snprintf(
                base, sizeof(base), "fp_constants[%u]", constant_slot);
        else if (has_k)
            snprintf(base, sizeof(base), "float4(%g,%g,%g,%g)",
                     k[0], k[1], k[2], k[3]);
        else
            snprintf(base, sizeof(base), "float4(0,0,0,0)");
    }

    char swz[160];
    snprintf(swz, sizeof(swz), "(%s).%s", base, s->swz);

    const char* pre = "";
    const char* post = "";
    if (s->abs)    { pre = "abs("; post = ")"; }
    if (s->negate) snprintf(buf, bufsz, "-(%s%s%s)", pre, swz, post);
    else           snprintf(buf, bufsz, "%s%s%s", pre, swz, post);
}

/* Dest write-mask letters from OPDEST bits 9..12 (X..W). */
static void dest_mask(u32 op0, char* m)
{
    int n = 0;
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 0))) m[n++] = 'x';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 1))) m[n++] = 'y';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 2))) m[n++] = 'z';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 3))) m[n++] = 'w';
    if (n == 0) { m[0] = 'x'; m[1] = 'y'; m[2] = 'z'; m[3] = 'w'; n = 4; }
    m[n] = '\0';
}

/* CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS: when set, fragment output is fp32
 * (color = r0); when clear, output is fp16 (color = h0). */
#define FP_CTRL_32BIT_EXPORTS 0x40u

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size)
{
    return rsx_fp_decompile_ex(ucode, max_bytes, ctrl, 0u, out, out_size);
}

static int rsx_fp_decompile_internal(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    int buffered, char* out, u32 out_size, u32* out_constant_count)
{
    if (!ucode || !out || out_size == 0) return -1;
    if(!rsx_fp_validate_control_flow(ucode,max_bytes))return -1;

    u32 buffered_constant_count = 0;
    if (buffered) {
        rsx_fp_constant_block constants;
        const int result =
            rsx_fp_collect_constants(ucode, max_bytes, &constants);
        if (result < 0)
            return -1;
        buffered_constant_count = constants.count;
    }
    if (out_constant_count)
        *out_constant_count = buffered_constant_count;

    Out o = { out, out_size, 0, 1 };

    /* Preamble: PSInput (position, col0, col1, fog, tc0..tc7); temp/half
     * register files; texture+sampler banks for TEX. */
    out_puts(&o,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
        "};\n");
    /* Texture bank. With no cube units (the default) emit a single array
     * declaration for the 2D-only case. When any unit
     * is a cubemap, declare each unit individually at its t-register so the
     * cube units can be TextureCube while the rest stay Texture2D. */
    if (tex_cube_mask == 0) {
        out_puts(&o, "Texture2D    rsx_tex[16] : register(t0);\n");
    } else {
        char decl[64];
        for (u32 u = 0; u < 16; u++) {
            snprintf(decl, sizeof(decl), "%s rsx_tex%u : register(t%u);\n",
                     ((tex_cube_mask >> u) & 1u) ? "TextureCube" : "Texture2D  ", u, u);
            out_puts(&o, decl);
        }
    }
    out_puts(&o,
        "SamplerState rsx_samp[16] : register(s0);\n"
    );
    if (buffered) {
        char constants_decl[192];
        snprintf(
            constants_decl, sizeof(constants_decl),
            "cbuffer PSConstants : register(b1) {\n"
            "    float4 fp_constants[%u];\n"
            "    float4 fp_alpha;\n"
            "};\n",
            buffered_constant_count ? buffered_constant_count : 1u);
        out_puts(&o, constants_decl);
    }
    out_puts(&o,
        "float4 main(PSInput input) : SV_TARGET {\n"
        "    float4 r[48]; float4 h[48];\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* Condition-code registers for exec_if predication / set_cond. Reset
         * to 0 (NV40 CC power-up state). Unused unless the program predicates
         * instructions. */
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");

    int wrote_r0 = 0, wrote_h0 = 0;
    int count = 0;
    u32 off = 0;
    u32 constant_slot = 0;
    fp_flow_scope flow_stack[32];u32 flow_depth=0;

    while (off + 16 <= max_bytes) {
        while(flow_depth && flow_stack[flow_depth-1].end==off) {out_puts(&o,"    }\n");--flow_depth;}
        if(flow_depth && flow_stack[flow_depth-1].alternative==off) {
            out_puts(&o,"    } else {\n");flow_stack[flow_depth-1].alternative=0;
        }
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 full_opcode = fp_full_opcode(w0, w2);
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;

        /* Execution condition (exec_if) + condition-code write (set_cond).
         * All-set == unconditional, none-set == never, otherwise a
         * per-component select of the result vs the destination, gated by a
         * sign test of the cond-swizzled CC register. */
        int exec_lt   = (w1 & FP_EXEC_IF_LT) ? 1 : 0;
        int exec_eq   = (w1 & FP_EXEC_IF_EQ) ? 1 : 0;
        int exec_gr   = (w1 & FP_EXEC_IF_GR) ? 1 : 0;
        int is_uncond = (exec_lt && exec_eq && exec_gr);
        int is_never  = (!exec_lt && !exec_eq && !exec_gr);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        u32 cc_read   = (w1 & FP_COND_REG)     ? 1u : 0u;
        u32 cc_write  = (w1 & FP_COND_MOD_REG) ? 1u : 0u;
        static const char comp_c[4] = {'x','y','z','w'};
        char cswz[5];
        for (int ci = 0; ci < 4; ci++)
            cswz[ci] = comp_c[(w1 >> (FP_COND_SWZ_SHIFT + 2 * ci)) & 3];
        cswz[4] = '\0';
        /* HLSL comparison operator for the exec_if sign mask (the
         * SGE/SLE/SNE/SGT/SLT/SEQ selection). */
        const char* cmp_op;
        if      (exec_gr && exec_eq) cmp_op = ">=";
        else if (exec_lt && exec_eq) cmp_op = "<=";
        else if (exec_gr && exec_lt) cmp_op = "!=";
        else if (exec_gr)            cmp_op = ">";
        else if (exec_lt)            cmp_op = "<";
        else                         cmp_op = "==";  /* exec_eq only */

        /* RET is a top-level, optionally predicated early exit on NV40. It
         * does not consume source operands or an inline constant even when
         * the overlaid flow-control fields happen to look like CONST source
         * encodings. A main-block RET stays in linear flow and returns under
         * this same any-component execution condition. Return the currently
         * selected RSX color register so writes completed before RET are
         * preserved. */
        if (full_opcode == OP_RET) {
            const char* result_reg;
            if (ctrl == RSX_FP_CTRL_AUTO)
                result_reg = (wrote_r0 || !wrote_h0) ? "r[0]" : "h[0]";
            else
                result_reg = (ctrl & FP_CTRL_32BIT_EXPORTS) ? "r[0]" : "h[0]";

            if (is_never) {
                out_puts(&o, "    /* RET exec_if=none: return suppressed */\n");
            } else if (is_uncond) {
                char line[64];
                snprintf(line, sizeof(line), "    return %s;\n", result_reg);
                out_puts(&o, line);
            } else {
                char line[160];
                snprintf(line, sizeof(line),
                         "    if (any(cc%u.%s %s (float4)0)) return %s;\n",
                         cc_read, cswz, cmp_op, result_reg);
                out_puts(&o, line);
            }
            if (w0 & FP_END) break;
            continue;
        }

        if(full_opcode==OP_IFE || full_opcode==OP_LOOP) {
            char condition[96],line[256];
            if(is_uncond || is_never)snprintf(condition,sizeof(condition),"%s",is_uncond?"true":"false");
            else snprintf(condition,sizeof(condition),"any(cc%u.%s %s (float4)0)",cc_read,cswz,cmp_op);
            const u32 end=w3*4,alternative=full_opcode==OP_IFE?(w2&0x7fffffffu)*4:end;
            if(full_opcode==OP_IFE)snprintf(line,sizeof(line),"    if (%s) {\n",condition);
            else snprintf(line,sizeof(line),"    if (%s) [loop] for (int loop%u=%u; loop%u<%u; loop%u+=%u) {\n",
                condition,flow_depth,(w2>>10)&255,flow_depth,(w2>>2)&255,flow_depth,(w2>>19)&255);
            out_puts(&o,line);flow_stack[flow_depth++]=(fp_flow_scope){end,alternative<end?alternative:0};
            continue;
        }

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, 0,                &s2);

        /* Inline constant: any CONST source pulls the next 16 bytes as a
         * float4 literal and advances past it. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        if (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
            s2.type == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_read_word(ucode + off + i * 4);
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
            }
            has_k = 1;
        }

        char a[200], b[200], c[200];
        emit_src(
            &s0, input_src, k, has_k, buffered, constant_slot,
            a, sizeof(a));
        emit_src(
            &s1, input_src, k, has_k, buffered, constant_slot,
            b, sizeof(b));
        emit_src(
            &s2, input_src, k, has_k, buffered, constant_slot,
            c, sizeof(c));
        if (has_k)
            constant_slot++;

        /* Build the RHS expression for this opcode. */
        char rhs[640];
        int handled = 1;
        switch (opcode) {
        case OP_NOP: rhs[0] = '\0'; handled = 0; break;
        case OP_MOV: snprintf(rhs, sizeof(rhs), "%s", a); break;
        case OP_MUL: snprintf(rhs, sizeof(rhs), "(%s) * (%s)", a, b); break;
        case OP_ADD: snprintf(rhs, sizeof(rhs), "(%s) + (%s)", a, b); break;
        case OP_MAD: snprintf(rhs, sizeof(rhs), "(%s) * (%s) + (%s)", a, b, c); break;
        case OP_DP3: snprintf(rhs, sizeof(rhs), "dot((%s).xyz, (%s).xyz)", a, b); break;
        case OP_DP4: snprintf(rhs, sizeof(rhs), "dot((%s), (%s))", a, b); break;
        case OP_MIN: snprintf(rhs, sizeof(rhs), "min((%s), (%s))", a, b); break;
        case OP_MAX: snprintf(rhs, sizeof(rhs), "max((%s), (%s))", a, b); break;
        case OP_FRC: snprintf(rhs, sizeof(rhs), "frac(%s)", a); break;
        case OP_FLR: snprintf(rhs, sizeof(rhs), "floor(%s)", a); break;
        case OP_RCP: snprintf(rhs, sizeof(rhs), "(1.0 / (%s).x)", a); break;
        /* Real RSX RSQ ignores the input's sign: 1/sqrt(|x|), not 1/sqrt(x).
         * The strict form yields NaN in normal-map Z-reconstruction
         * (1-x^2-y^2 is negative for many pixels) and poisons the whole
         * lighting chain. */
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt(abs((%s).x))", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        /* Real RSX DIVSQ shares RSQ's sign-ignoring sqrt on the denominator
         * and forces the result to exactly 0, component-wise, wherever the
         * numerator component is 0, even if the denominator is also 0. A true
         * per-component select (HLSL vector `?:`, not lerp/mix) avoids NaN
         * contamination from the 0/0 branch, which a blend would let through. */
        case OP_DIVSQ:
            snprintf(rhs, sizeof(rhs),
                     "(abs(%s) > 0.0 ? (%s) / sqrt(abs((%s).x)) : (float4)0.0)",
                     a, a, b);
            break;
        case OP_DP2: snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy)", a, b); break;
        case OP_NRM: snprintf(rhs, sizeof(rhs), "float4(normalize((%s).xyz), 1.0)", a); break;
        case OP_LRP: snprintf(rhs, sizeof(rhs), "lerp((%s), (%s), (%s))", c, b, a); break;
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX:
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Cubemap: sample with the full 3-component direction vector. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (tex_cube_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            break;
        case OP_TXP:
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Projective divide is meaningless for a cube lookup; sample
                 * the direction directly. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (tex_cube_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            break;
        case OP_KIL:
            /* Fragment kill. Predicated by the same exec_if condition as any
             * other instruction; unconditional encodings discard outright.
             * Without it every alpha-cutout material renders as a solid
             * quad. */
            if (is_never) {
                rhs[0] = '\0';
            } else if (is_uncond) {
                out_puts(&o, "    discard;\n");
            } else {
                char kil[128];
                snprintf(kil, sizeof(kil),
                         "    if (any(cc%u.%s %s (float4)0)) discard;\n",
                         cc_read, cswz, cmp_op);
                out_puts(&o, kil);
            }
            handled = 0;   /* no destination write */
            break;
        default:
            out_puts(&o, "    /* TODO: unhandled FP opcode ");
            out_puts(&o, rsx_fp_opcode_name(opcode));
            out_puts(&o, " */\n");
            handled = 0;
            break;
        }

        int has_dest = !(w0 & FP_OUT_NONE);
        if (handled && (has_dest || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* reg = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            const char* sat = (w0 & FP_OUT_SAT) ? " _v = saturate(_v);" : "";
            /* NV40 per-instruction result-scale modifier (SRC1 word bits
             * 28-30): 1/2/3 = *2/*4/*8, 5/6/7 = /2//4//8; applied to the
             * result before saturate. Ignoring it leaves e.g. a two-tap box
             * filter (/2 on the final ADD) unaveraged, a flat 2x gain. */
            u32 res_scale = (w2 >> 28) & 0x7u;
            static const char* scale_txt[8] = { "", " * 2.0", " * 4.0", " * 8.0",
                                                "", " / 2.0", " / 4.0", " / 8.0" };

            /* Broadcast the result to float4 first so the write-mask picks the
             * corresponding components: `dst.w = rhs` would HLSL-truncate a
             * vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            if (has_dest && is_uncond && !set_cond) {
                /* Common unconditional path. */
                char line[800];
                snprintf(line, sizeof(line),
                         "    { float4 _v = (float4)(%s)%s;%s %s[%u].%s = _v.%s; }\n",
                         rhs, scale_txt[res_scale], sat, reg, dst_idx, m, m);
                out_puts(&o, line);
            } else {
                /* Predicated and/or CC-writing instruction. */
                char buf[1200];
                snprintf(buf, sizeof(buf),
                         "    { float4 _v = (float4)(%s)%s;%s\n",
                         rhs, scale_txt[res_scale], sat);
                out_puts(&o, buf);
                if (has_dest) {
                    if (is_uncond) {
                        snprintf(buf, sizeof(buf),
                                 "      %s[%u].%s = _v.%s;\n", reg, dst_idx, m, m);
                    } else if (is_never) {
                        snprintf(buf, sizeof(buf),
                                 "      /* exec_if=none: write suppressed */\n");
                    } else {
                        /* Per-component select gated by the CC sign test.
                         * lerp(dst, _v, mask) == mask ? _v : dst since the
                         * mask is exactly 0.0/1.0 from the bool->float cast. */
                        snprintf(buf, sizeof(buf),
                                 "      float4 _p = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      %s[%u].%s = lerp(%s[%u].%s, _v.%s, _p.%s);\n",
                                 cc_read, cswz, cmp_op,
                                 reg, dst_idx, m, reg, dst_idx, m, m, m);
                    }
                    out_puts(&o, buf);
                }
                if (set_cond) {
                    /* CC receives the destination value (post-select), masked;
                     * for no-dest set_cond it receives the raw result. */
                    if (has_dest)
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = %s[%u].%s;\n", cc_write, m, reg, dst_idx, m);
                    else
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = _v.%s;\n", cc_write, m, m);
                    out_puts(&o, buf);
                }
                out_puts(&o, "    }\n");
            }

            if (has_dest && !dst_half && dst_idx == 0) wrote_r0 = 1;
            if (has_dest && dst_half  && dst_idx == 0) wrote_h0 = 1;
        }

        if (w0 & FP_END) break;
    }

    /* Fragment color output register selection. The NV40 hardware picks the
     * output from the SHADER_CONTROL word's 32_BITS_EXPORTS bit, not from
     * whichever temp happened to be written: fp32-export programs output r0,
     * fp16-export programs (the default) output h0. Many real material/lighting
     * shaders accumulate into fp32 temps (r2/r3/r4) but write their final
     * color to h0, so a "wrote_r0 => return r0" rule would return a stale
     * intermediate. */
    if (ctrl == RSX_FP_CTRL_AUTO) {
        /* No control word (standalone tests): use the written-register heuristic. */
        if (wrote_r0 || !wrote_h0)
            out_puts(&o, "    return r[0];\n}\n");
        else
            out_puts(&o, "    return h[0];\n}\n");
    } else if (ctrl & FP_CTRL_32BIT_EXPORTS) {
        out_puts(&o, "    return r[0];\n}\n");
    } else {
        out_puts(&o, "    return h[0];\n}\n");
    }

    if (!o.ok) return -1;
    return count;
}

int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, out, out_size, NULL);
}

int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 1, out, out_size,
        out_constant_count);
}

static float fp_half_to_float(u16 h)
{
    const u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exponent = (h >> 10) & 0x1Fu;
    u32 mantissa = h & 0x03FFu;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format)
{
    /* NV4097 surface color formats: 14=W16Z16Y16X16,
     * 15=W32Z32Y32X32, 16=X32. */
    if (surface_color_format == 14u)
        return fp_half_to_float((u16)(raw & 0xFFFFu));
    if (surface_color_format == 15u || surface_color_format == 16u) {
        float value;
        memcpy(&value, &raw, sizeof(value));
        return value;
    }
    return (float)(raw & 0xFFu) / 255.0f;
}

int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp = func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    static const char* pass_expr[8] = {
        "false", "_rsx_out.a < _rsx_alpha_ref",
        "_rsx_out.a == _rsx_alpha_ref", "_rsx_out.a <= _rsx_alpha_ref",
        "_rsx_out.a > _rsx_alpha_ref", "_rsx_out.a != _rsx_alpha_ref",
        "_rsx_out.a >= _rsx_alpha_ref", "true"
    };
    char suffix[512];
    const int n = snprintf(suffix, sizeof(suffix),
        "    float4 _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = %.9g;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        output, (double)ref, pass_expr[cmp]);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp =
        func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    static const char* pass_expr[8] = {
        "false", "_rsx_out.a < _rsx_alpha_ref",
        "_rsx_out.a == _rsx_alpha_ref", "_rsx_out.a <= _rsx_alpha_ref",
        "_rsx_out.a > _rsx_alpha_ref", "_rsx_out.a != _rsx_alpha_ref",
        "_rsx_out.a >= _rsx_alpha_ref", "true"
    };
    char suffix[512];
    const int n = snprintf(
        suffix, sizeof(suffix),
        "    float4 _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = fp_alpha.x;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        output, pass_expr[cmp]);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

int rsx_fp_apply_texcoord_control(
    char* hlsl, u32 out_size, u32 texcoord_2d_mask)
{
    if (!hlsl || !out_size || (texcoord_2d_mask & ~0xFFu))
        return -1;
    if (!texcoord_2d_mask)
        return 0;
    static const char marker[] =
        "float4 main(PSInput input) : SV_TARGET {\n";
    char* const main = strstr(hlsl, marker);
    if (!main)
        return -1;
    char insert[768];
    size_t used = 0;
    for (u32 unit = 0; unit < 8u; ++unit) {
        if (!(texcoord_2d_mask & (1u << unit)))
            continue;
        const int n = snprintf(
            insert + used, sizeof(insert) - used,
            "    input.tc%u = float4(input.tc%u.xy, 0.0, "
            "input.position.w);\n",
            unit, unit);
        if (n < 0 || (size_t)n >= sizeof(insert) - used)
            return -1;
        used += (size_t)n;
    }
    char* const at = main + sizeof(marker) - 1u;
    const size_t tail = strlen(at) + 1u;
    const size_t prefix = (size_t)(at - hlsl);
    if (prefix + used + tail > out_size)
        return -1;
    memmove(at + used, at, tail);
    memcpy(at, insert, used);
    return 1;
}

int rsx_fp_apply_shader_window(
    char* hlsl, u32 out_size, u32 shader_window)
{
    if (!hlsl || !out_size || (shader_window & 0xFFF00000u))
        return -1;
    const u32 height = shader_window & 0xFFFu;
    const u32 origin = (shader_window >> 12) & 0xFu;
    const u32 center = (shader_window >> 16) & 0xFu;
    if (origin > 1u || center > 1u)
        return -1;
    if (!origin && !center)
        return 0;
    static const char marker[] =
        "float4 main(PSInput input) : SV_TARGET {\n";
    char* const main = strstr(hlsl, marker);
    if (!main)
        return -1;
    char insert[256];
    int n;
    if (origin) {
        n = snprintf(
            insert, sizeof(insert),
            "    input.position.x = input.position.x%s;\n"
            "    input.position.y = %.9gf - input.position.y%s;\n",
            center ? " - 0.5" : "", (double)height,
            center ? " - 0.5" : "");
    } else {
        n = snprintf(
            insert, sizeof(insert),
            "    input.position.x = input.position.x%s;\n"
            "    input.position.y = input.position.y%s;\n",
            center ? " - 0.5" : "", center ? " - 0.5" : "");
    }
    if (n < 0 || (u32)n >= sizeof(insert))
        return -1;
    char* const at = main + sizeof(marker) - 1u;
    const size_t tail = strlen(at) + 1u;
    const size_t prefix = (size_t)(at - hlsl);
    if (prefix + (size_t)n + tail > out_size)
        return -1;
    memmove(at + n, at, tail);
    memcpy(at, insert, (size_t)n);
    return 1;
}
