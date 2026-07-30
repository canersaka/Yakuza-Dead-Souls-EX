/*
 * ps3recomp - compact CPU vertex-fetch planning for the live RSX renderer
 *
 * The legacy renderer expands every guest attribute into a 16 x float4
 * vertex.  This helper preserves the same conversion rules while allowing a
 * caller to fetch only the attributes statically read by the active vertex
 * program.  It is D3D12-independent so format/bounds behavior can be tested
 * without creating a device.
 */
#ifndef PS3RECOMP_RSX_VERTEX_COMPACT_H
#define PS3RECOMP_RSX_VERTEX_COMPACT_H

#include "rsx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const u8* (*rsx_vertex_guest_ptr_fn)(
    void* user, u32 location, u32 offset, u32 min_bytes);

typedef struct rsx_vertex_layout_plan {
    u32 mask;
    u32 stride;
    u8 count;
    u8 attrs[RSX_DSP_NUM_VERTEX_ATTR];
    s8 slot_by_attr[RSX_DSP_NUM_VERTEX_ATTR];
} rsx_vertex_layout_plan;

typedef struct rsx_vertex_ref {
    u32 vertex_id;
    u32 base_index;
} rsx_vertex_ref;

typedef struct rsx_vertex_fetch_attr {
    rsx_dsp_vertex_attr desc;
    u32 elem_size;
    u32 stride;
    float default_value[4];
    const u8* stable_base;
    u32 stable_first;
    u32 stable_last;
} rsx_vertex_fetch_attr;

typedef struct rsx_vertex_fetch_plan {
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_attr attr[RSX_DSP_NUM_VERTEX_ATTR];
    u32 base_offset;
    u32 divider_mask;
    rsx_vertex_guest_ptr_fn guest_ptr;
    void* guest_user;
} rsx_vertex_fetch_plan;

/* Deterministic ascending-register layout; each selected register remains a
 * float4 and retains its original ATTR semantic index. */
void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask);

/* Decode all 16 descriptors/defaults once for a draw. */
void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user);

/* Attempt one bounds-checked contiguous mapping per used attribute.  Any
 * attribute whose complete span is not contiguous safely falls back to the
 * legacy per-element resolver in rsx_vertex_fetch_one. */
void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count);

/* Fetch one compact vertex in layout order.  Disabled attributes use their
 * RSX current/default value.  Enabled non-position attributes retain the
 * legacy fallback-to-default behavior on an unsupported/invalid fetch; an
 * invalid enabled ATTR0 remains fatal. */
int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out);

/* Public for focused format/endian tests. */
int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4]);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_VERTEX_COMPACT_H */
