/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) -> HLSL decompiler
 *
 * Translates an RSX vertex shader (NVIDIA NV40 "nvfx" vertex program, a VLIW
 * that co-issues one vector ALU op and one scalar ALU op per instruction) into
 * an HLSL vertex shader.
 *
 * Encoding: D0..D3 bitfields, with opcode tables as in Mesa nouveau
 * nvfx_shader.h. Words are stored little-endian, 4 words / 16 bytes per
 * instruction, no inline constants (VP constants live in a separate bank),
 * program ends at the D3.end bit.
 *
 * Self-contained: no D3D12 dependency, build/test standalone.
 */
#ifndef PS3RECOMP_RSX_VP_DECOMPILER_H
#define PS3RECOMP_RSX_VP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of instructions in the program at `ucode` (each 16 bytes), up to and
 * including the one with the D3.end bit, bounded by max_bytes. 0 if none. */
u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes);

typedef struct rsx_vp_input_analysis {
    u32 input_mask;
    int exact;
} rsx_vp_input_analysis;

typedef struct rsx_vp_native_support_analysis {
    u32 unsupported_vec_mask;
    u32 unsupported_sca_mask;
    u32 missing_vtex_mask;
    u32 conditional_tests;
    u32 flow_instructions;
    u32 invalid_branch_targets;
    u32 instruction_count;
    u32 terminated;
} rsx_vp_native_support_analysis;

/* Option for rsx_vp_analyze_native_support_control_options: accept programs
 * that combine BRI/BRB flow with vertex-texture fetches. Without it such
 * programs are reported as unsupported. */
#define RSX_VP_NATIVE_COHERENT_SECTION_FLOW_TXL 1u

/* Report the guest input registers statically read by the program.  `exact`
 * is zero and input_mask is 0xFFFF when an unsupported opcode/control-flow
 * construct makes the result unsafe to narrow.  Returns the instruction
 * count, or -1 for invalid/non-terminated input. */
int rsx_vp_analyze_inputs(const u8* ucode, u32 max_bytes,
                          rsx_vp_input_analysis* analysis);

/* Conservative eligibility gate for native execution.  Returns one only
 * when the terminated program uses instruction and condition semantics the
 * current HLSL decompiler models exactly.  In particular, this rejects all
 * branch/call/return opcodes and non-trivial condition-code tests even when
 * they carry no ordinary scalar write mask. */
int rsx_vp_program_is_native_supported(const u8* ucode, u32 max_bytes);

/* Binding-aware eligibility gate. TXL is accepted only when its exact
 * vertex-texture unit is present in vtex_mask; all other semantics match the
 * conservative gate above. */
int rsx_vp_program_is_native_supported_ex(const u8* ucode, u32 max_bytes,
                                          u32 vtex_mask);

/* Start-slot-aware form used by live programs. It additionally accepts
 * exact forward BRI/BRB control flow whose absolute targets remain inside
 * the program. Other flow opcodes and cyclic/backward targets are rejected. */
int rsx_vp_program_is_native_supported_control_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 start_slot);

/* Detailed form of the same conservative eligibility gate. It is intended
 * for bounded aggregate diagnostics and offline coverage analysis: no
 * allocation or output occurs here. Returns one iff every reported failure
 * field is zero and the program terminated within max_bytes. */
int rsx_vp_analyze_native_support(
    const u8* ucode, u32 max_bytes, u32 vtex_mask,
    rsx_vp_native_support_analysis* analysis);
int rsx_vp_analyze_native_support_control(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 start_slot,
    rsx_vp_native_support_analysis* analysis);
int rsx_vp_analyze_native_support_control_options(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 start_slot,
    u32 options, rsx_vp_native_support_analysis* analysis);

/* Decompile an NV40 vertex program into an HLSL vertex shader.
 *   ucode    : VP bytecode (little-endian words).
 *   max_bytes: safety bound.
 *   out      : caller buffer for generated HLSL (NUL-terminated).
 *   out_size : size of out in bytes.
 * Returns instruction count (>=0), or -1 on error (null args / overflow).
 * Emits `VSOutput main(VSInput input)`: 16 float4 inputs (ATTR0..15), a
 * `cbuffer VPConst : register(b0)` with 512 vec4 constants + vp_posscale/
 * vp_posoffset (the RSX viewport transform mapped to D3D clip space; the
 * caller computes them per draw), SV_Position + COLOR0/1 + FOG + TEXCOORD0..7
 * varyings routed per the NV40 output register map (o0/o1/o2/o5/o7..o14).
 * Exact condition-code tests and forward in-program BRI/BRB are modeled by
 * the start-slot-aware live form. BRA/call/return and cyclic flow remain
 * unsupported. TXL is stubbed to zero unless rsx_vp_decompile_ex receives a
 * bound-unit mask. */
int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size);

/* Bit N in vtex_mask declares NV40 2D vertex-texture unit N at t(16+N),
 * sampler sN, and turns TXL for that unit into SampleLevel(..., LOD 0).
 * Unmasked units retain the defined-zero fallback. */
int rsx_vp_decompile_ex(const u8* ucode, u32 max_bytes, u32 vtex_mask,
                        char* out, u32 out_size);
/* Like rsx_vp_decompile_ex, and also accepts forward BRI/BRB flow combined
 * with TXL. Branch targets are absolute RSX instruction slots. */
int rsx_vp_decompile_control_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 start_slot,
    char* out, u32 out_size);

/* Compact-input variant.  The generated VSInput declares only the original
 * ATTRn semantics selected by input_mask.  The mask is checked against a
 * fresh static analysis; any uncertainty or missing referenced bit falls
 * back to all 16 inputs. */
int rsx_vp_decompile_compact_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    char* out, u32 out_size);
int rsx_vp_decompile_compact_control_ex(
    const u8* ucode, u32 max_bytes, u32 vtex_mask, u32 input_mask,
    u32 start_slot, char* out, u32 out_size);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VP_DECOMPILER_H */
