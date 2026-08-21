/*
 * ps3recomp - true GPU vertex pulling for the live RSX renderer (codegen)
 *
 * The compact CPU path (rsx_vertex_compact.c) decodes every used guest
 * attribute on the CPU and uploads a packed float4 payload per draw.  This
 * module generates the HLSL for the GPU to do that work itself: the raw
 * guest address spaces are bound as ByteAddressBuffer SRVs (backed by the
 * persistent rsx_gpu_mirror), and per-attribute fetch functions compute the
 * RSX element address from the vertex reference, base offset/index, stride,
 * frequency-divider state and location, then byte-swap and decode the RSX
 * attribute format — exactly mirroring rsx_vertex_decode_element().
 *
 * This is genuine vertex pulling: it replaces the CPU *attribute fetch*.
 * It is unrelated to vertex-texture sampling (rsx_vtexN, an existing GPU
 * feature) and does not change how the translated vertex program executes.
 *
 * Scope and fallback:
 *   - The plan marks any used attribute whose (type, size) is outside the
 *     allowed set as unsupported; the caller then keeps the existing compact
 *     CPU path for the whole draw.  All seven RSX types are implemented and
 *     bit-parity-tested offline, but callers can restrict the set.
 *   - Element sources: ARRAYS (element = first + SV_VertexID; also serves a
 *     host-built raw-index buffer with first = 0), or in-shader fetch of the
 *     guest big-endian u16/u32 index array.  Primitive-restart draws must
 *     not use the in-shader index source (the CPU cut-scan owns restart);
 *     the caller keeps building its filtered index list and pulls only
 *     attributes.
 *   - Per-element fetches that fall outside the bound space produce the
 *     attribute's RSX default value in-shader, mirroring the CPU fallback
 *     for enabled attributes.  (The CPU path drops a draw whose enabled
 *     ATTR0 is unfetchable; the shader cannot drop a draw, so integration
 *     must pre-validate ATTR0's span — rsx_vertex_fetch_plan_prepare
 *     already computes it.)
 *
 * The generated text depends only on each used attribute's (type, size) and
 * the pulled mask — rsx_vertex_pull_signature() keys a shader cache — while
 * offsets, strides, frequencies, locations, defaults and draw parameters
 * arrive through a cbuffer (b1) so one shader serves many draws.
 *
 * Register allocation (VS stage): t20/t21 = local/main guest memory SRVs,
 * b1 = the pull cbuffer.  t0..t15 (fragment textures), t16..t19 (vertex
 * textures) and b0 (VPConst) keep their existing meanings.
 *
 * The whole feature is default-OFF behind YZ_RSX_GPU_PULL; nothing in the
 * live path consumes this module until integration wires it up.
 */
#ifndef PS3RECOMP_RSX_VERTEX_PULL_H
#define PS3RECOMP_RSX_VERTEX_PULL_H

#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Element source selector (cbuffer `source`). */
#define RSX_PULL_SOURCE_ARRAYS     0u
#define RSX_PULL_SOURCE_INDEX_U16  1u
#define RSX_PULL_SOURCE_INDEX_U32  2u

/* Allowed-type bitmask: bit (1 << RSX_VTX_TYPE_*). */
#define RSX_PULL_TYPES_ALL 0xFEu    /* SNORM16..UINT8, i.e. types 1..7 */

typedef struct rsx_vertex_pull_attr {
    rsx_dsp_vertex_attr desc;
    u32 elem_size;
    u32 stride;                     /* desc.stride or packed elem_size */
    float default_value[4];
    u8 pulled;                      /* fetched from guest memory       */
} rsx_vertex_pull_attr;

typedef struct rsx_vertex_pull_plan {
    rsx_vertex_layout_plan layout;  /* attrs the vertex program reads  */
    rsx_vertex_pull_attr attr[RSX_DSP_NUM_VERTEX_ATTR];
    u32 base_offset;                /* VERTEX_DATA_BASE_OFFSET         */
    u32 divider_mask;               /* FREQUENCY_DIVIDER_OPERATION     */
    u32 pulled_mask;
    u32 unsupported_mask;           /* nonzero => keep the CPU path    */
} rsx_vertex_pull_plan;

/* CPU-side image of `cbuffer YzVertexPull : register(b1)`.
 * attr_cfg[n] = {offset, stride, frequency, flags}; flags bit0 = main
 * location, bit1 = modulo divider op. */
typedef struct rsx_vertex_pull_constants {
    u32 base_offset;
    u32 base_index;      /* applied in-shader for frequency <= 1 attrs */
    u32 first;           /* first vertex / first index-array slot      */
    u32 source;          /* RSX_PULL_SOURCE_*                          */
    u32 index_offset;    /* guest index-array byte offset              */
    u32 index_location;
    u32 mem_size[2];     /* bound SRV byte sizes (bounds/default)      */
    u32 attr_cfg[RSX_DSP_NUM_VERTEX_ATTR][4];
    float attr_default[RSX_DSP_NUM_VERTEX_ATTR][4];
} rsx_vertex_pull_constants;

#define RSX_PULL_ATTR_FLAG_MAIN    1u
#define RSX_PULL_ATTR_FLAG_MODULO  2u

/* Decode the 16 attribute descriptors for the used layout and decide
 * pullability against `allowed_types` (RSX_PULL_TYPES_ALL for everything
 * implemented).  Returns 1 when every used, enabled attribute can be
 * pulled (unsupported_mask == 0). */
int rsx_vertex_pull_plan_init(rsx_vertex_pull_plan* plan,
                              const rsx_dispatch* rsx,
                              const rsx_vertex_layout_plan* layout,
                              u32 allowed_types);

/* Shader-cache key: hashes exactly what the generated text depends on
 * (used mask + per-attr type/size/pulled). */
u64 rsx_vertex_pull_signature(const rsx_vertex_pull_plan* plan);

/* Emit the global-scope HLSL block: SRV + cbuffer declarations, load/
 * decode helpers, and one yz_pull_attrN() per pulled attribute.  Returns
 * the text length, or -1 on overflow. */
int rsx_vertex_pull_emit_globals(const rsx_vertex_pull_plan* plan,
                                 char* out, u32 out_size);

/* Emit the statements that fill `v[0..15]` inside the shader body from the
 * system vertex id expression `vid_expr` (e.g. "yz_sysvid").  Non-pulled
 * attributes read their cbuffer default. */
int rsx_vertex_pull_emit_loads(const rsx_vertex_pull_plan* plan,
                               const char* vid_expr, char* out,
                               u32 out_size);

/* Fill the b1 cbuffer image for one draw. */
void rsx_vertex_pull_fill_constants(const rsx_vertex_pull_plan* plan,
                                    u32 base_index, u32 first, u32 source,
                                    u32 index_offset, u32 index_location,
                                    u32 mem_size_local, u32 mem_size_main,
                                    rsx_vertex_pull_constants* out);

/* Convenience: decompile the vertex program with pulled inputs (wraps
 * rsx_vp_decompile_pull_ex with this plan's globals/loads).  Returns the
 * instruction count or -1. */
int rsx_vertex_pull_decompile(const rsx_vertex_pull_plan* plan,
                              const u8* ucode, u32 max_bytes, u32 vtex_mask,
                              char* out, u32 out_size);

/* Feature flag YZ_RSX_GPU_PULL (default OFF, read once).  Integration
 * gates every live consumer of this module and of the GPU mirror on it. */
int rsx_vertex_pull_flag_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VERTEX_PULL_H */
