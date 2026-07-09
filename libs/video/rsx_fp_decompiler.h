/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX fragment shader (NVIDIA NV40 "nvfx" fragment program
 * encoding) into an HLSL pixel shader source string suitable for D3DCompile.
 *
 * See docs/RSX_FRAGMENT_PROGRAM.md for the instruction encoding reference
 * (sourced from Mesa/nouveau nvfx_shader.h). This module is self-contained:
 * it has no D3D12 dependency and can be built/tested standalone.
 */

#ifndef PS3RECOMP_RSX_FP_DECOMPILER_H
#define PS3RECOMP_RSX_FP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read one fragment-program word from guest (PS3) memory: big-endian load
 * followed by a 16-bit half-word swap, yielding a host-order instruction
 * word. The decoder operates on host-order words. */
u32 rsx_fp_read_word(const u8* p);

/* Decompile an NV40 fragment program into an HLSL pixel shader.
 *
 *   ucode     : pointer to the fragment-program bytecode in guest memory
 *               (big-endian, half-word-swapped — i.e. exactly as the RSX
 *               reads it; this function applies rsx_fp_read_word internally).
 *   max_bytes : safety bound on how far to read (program stops at PROGRAM_END
 *               or when this many bytes are consumed).
 *   out       : caller buffer for the generated HLSL (NUL-terminated).
 *   out_size  : size of `out` in bytes.
 *
 * Returns the number of instructions decoded (>=0) on success, or -1 on
 * error (null args, output overflow). The emitted entry point is
 * `float4 main(PSInput input) : SV_TARGET`, matching the backend's
 * placeholder PSInput { float4 position : SV_POSITION; float4 col : COLOR; }.
 *
 * Texture sampling and full input plumbing are partial — see the doc's
 * "Integration TODO". Unhandled opcodes emit a comment and a safe default so
 * the shader still compiles.
 *
 *   ctrl : the NV4097_SET_SHADER_CONTROL word (reg 0x1D60) for this program.
 *          Bit 0x40 (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) selects the
 *          fragment-output register set: clear => fp16 output (color = h0),
 *          set => fp32 output (color = r0). Pass RSX_FP_CTRL_AUTO when no
 *          control word is available (standalone tests) to fall back to the
 *          legacy "whichever of r0/h0 was written" heuristic. */
#define RSX_FP_CTRL_AUTO 0xFFFFFFFFu
int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size);

/* Return the mnemonic for an NV40 fragment opcode (or "?" if unknown).
 * Useful for disassembly/logging. */
const char* rsx_fp_opcode_name(u32 opcode);

/* Total byte length of the fragment program at `ucode` (including inline
 * CONST slots), up to and including the PROGRAM_END instruction, bounded by
 * `max_bytes`. Returns 0 if no terminator is found within the bound. Shared
 * walk logic so callers (e.g. a shader cache hashing the bytecode) agree with
 * the decompiler on where a program ends. */
u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_FP_DECOMPILER_H */
