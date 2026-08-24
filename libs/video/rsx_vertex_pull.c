/*
 * ps3recomp - true GPU vertex pulling: plan + HLSL codegen
 *
 * See rsx_vertex_pull.h.  Every decode rule here mirrors
 * rsx_vertex_decode_element()/rsx_vertex_element_index() exactly; the WARP
 * parity test (libs/video/tests/test_vertex_pull_gpu.c) compares the two
 * implementations bit-for-bit per format.
 */
#include "rsx_vertex_pull.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pull_out {
    char* p;
    u32 cap;
    u32 len;
    int overflow;
} pull_out;

static void pull_emit(pull_out* o, const char* s)
{
    const u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) {
        o->overflow = 1;
        return;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

static void pull_emitf(pull_out* o, const char* fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    pull_emit(o, line);
}

/* Shared classification once plan->attr[] descriptors and defaults are
 * populated (from a dispatch register file or from decoded state). */
static int pull_plan_classify(rsx_vertex_pull_plan* plan, u32 allowed_types)
{
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_pull_attr* a = &plan->attr[attr];
        a->elem_size = rsx_vertex_attrib_size(a->desc.type, a->desc.size);
        a->stride = a->desc.stride ? a->desc.stride : a->elem_size;
        /* Same ATTR3 (diffuse-color) neutral-default quirk as
         * rsx_vertex_fetch_plan_init. */
        if (attr == 3 &&
            a->default_value[0] == 0.0f && a->default_value[1] == 0.0f &&
            a->default_value[2] == 0.0f && a->default_value[3] == 1.0f) {
            a->default_value[0] = 1.0f;
            a->default_value[1] = 1.0f;
            a->default_value[2] = 1.0f;
        }
        if (!(plan->layout.mask & (1u << attr)))
            continue;
        if (!a->desc.type || !a->desc.size)
            continue;   /* disabled attribute: cbuffer default */
        if (a->desc.type > 7u || a->desc.size > 4u || !a->elem_size ||
            !((allowed_types >> a->desc.type) & 1u)) {
            plan->unsupported_mask |= 1u << attr;
            continue;
        }
        a->pulled = 1;
        plan->pulled_mask |= 1u << attr;
    }
    return plan->unsupported_mask == 0;
}

int rsx_vertex_pull_plan_init(rsx_vertex_pull_plan* plan,
                              const rsx_dispatch* rsx,
                              const rsx_vertex_layout_plan* layout,
                              u32 allowed_types)
{
    if (!plan || !rsx || !layout)
        return 0;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_pull_attr* a = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &a->desc);
        rsx_dsp_vertex_default(rsx, attr, a->default_value);
    }
    return pull_plan_classify(plan, allowed_types);
}

int rsx_vertex_pull_plan_init_decoded(rsx_vertex_pull_plan* plan,
                                      const rsx_dsp_vertex_attr* attrs,
                                      const float defaults[][4],
                                      u32 base_offset, u32 divider_mask,
                                      const rsx_vertex_layout_plan* layout,
                                      u32 allowed_types)
{
    if (!plan || !attrs || !defaults || !layout)
        return 0;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = base_offset;
    plan->divider_mask = divider_mask;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_pull_attr* a = &plan->attr[attr];
        a->desc = attrs[attr];
        memcpy(a->default_value, defaults[attr], sizeof(a->default_value));
    }
    return pull_plan_classify(plan, allowed_types);
}

u64 rsx_vertex_pull_signature(const rsx_vertex_pull_plan* plan)
{
    if (!plan)
        return 0;
    u64 hash = 1469598103934665603ull;
    const u64 prime = 1099511628211ull;
    hash = (hash ^ plan->layout.mask) * prime;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->layout.mask & (1u << attr)))
            continue;
        const rsx_vertex_pull_attr* a = &plan->attr[attr];
        const u64 word = ((u64)attr << 12) | ((u64)a->pulled << 8) |
                         ((a->desc.type & 0xFu) << 4) |
                         (a->desc.size & 0xFu);
        hash = (hash ^ word) * prime;
    }
    return hash;
}

/* Component decode line for one baked (type, component) pair. */
static void pull_emit_component(pull_out* o, u32 type, u32 component)
{
    static const char lane[4] = {'x', 'y', 'z', 'w'};
    const char c = lane[component & 3u];
    switch (type) {
    case RSX_VTX_TYPE_FLOAT:
        pull_emitf(o,
            "    value.%c = asfloat(yz_load_be32(loc, addr + %uu));\n",
            c, component * 4u);
        break;
    case RSX_VTX_TYPE_HALF:
        pull_emitf(o,
            "    value.%c = yz_half(yz_load_be16(loc, addr + %uu));\n",
            c, component * 2u);
        break;
    case RSX_VTX_TYPE_UNORM8:
        pull_emitf(o,
            "    value.%c = (float)yz_load_u8(loc, addr + %uu) / 255.0;\n",
            c, component);
        break;
    case RSX_VTX_TYPE_UINT8:
        pull_emitf(o,
            "    value.%c = (float)yz_load_u8(loc, addr + %uu);\n",
            c, component);
        break;
    case RSX_VTX_TYPE_SNORM16:
        pull_emitf(o,
            "    value.%c = yz_snorm16(yz_load_be16(loc, addr + %uu));\n",
            c, component * 2u);
        break;
    case RSX_VTX_TYPE_SINT16:
        pull_emitf(o,
            "    value.%c = yz_sint16(yz_load_be16(loc, addr + %uu));\n",
            c, component * 2u);
        break;
    default:
        break;
    }
}

int rsx_vertex_pull_emit_globals(const rsx_vertex_pull_plan* plan,
                                 char* out, u32 out_size)
{
    if (!plan || !out || out_size < 1024)
        return -1;
    pull_out o = {out, out_size, 0, 0};
    out[0] = '\0';

    pull_emit(&o,
        /* Raw guest memory (persistent GPU mirror) + per-draw parameters.
         * misc0 = base_offset, base_index, first, source;
         * misc1 = index_offset, index_location, mem_size0, mem_size1. */
        "ByteAddressBuffer yz_guest_mem0 : register(t20);\n"
        "ByteAddressBuffer yz_guest_mem1 : register(t21);\n"
        "cbuffer YzVertexPull : register(b1) {\n"
        "    uint4 yz_pull_misc0;\n"
        "    uint4 yz_pull_misc1;\n"
        "    uint4 yz_attr_cfg[16];\n"
        "    float4 yz_attr_default[16];\n"
        "};\n"
        "#define yz_base_offset (yz_pull_misc0.x)\n"
        "#define yz_base_index  (yz_pull_misc0.y)\n"
        "#define yz_first       (yz_pull_misc0.z)\n"
        "#define yz_source      (yz_pull_misc0.w)\n"
        "uint yz_bswap32(uint v) {\n"
        "    return (v >> 24) | ((v >> 8) & 0xFF00u) |\n"
        "           ((v << 8) & 0xFF0000u) | (v << 24);\n"
        "}\n"
        "uint yz_load_word(uint loc, uint addr) {\n"
        "    return (loc != 0u) ? yz_guest_mem1.Load(addr)\n"
        "                       : yz_guest_mem0.Load(addr);\n"
        "}\n"
        /* Little-endian dword at any byte address; the shift guard keeps\n
         * the aligned case away from the undefined <<32. */
        "uint yz_load_le32(uint loc, uint addr) {\n"
        "    uint base = addr & ~3u;\n"
        "    uint sh = (addr & 3u) * 8u;\n"
        "    uint w0 = yz_load_word(loc, base);\n"
        "    if (sh == 0u) return w0;\n"
        "    uint w1 = yz_load_word(loc, base + 4u);\n"
        "    return (w0 >> sh) | (w1 << (32u - sh));\n"
        "}\n"
        "uint yz_load_be32(uint loc, uint addr) {\n"
        "    return yz_bswap32(yz_load_le32(loc, addr));\n"
        "}\n"
        "uint yz_load_be16(uint loc, uint addr) {\n"
        "    uint le = yz_load_le32(loc, addr) & 0xFFFFu;\n"
        "    return ((le & 0xFFu) << 8) | (le >> 8);\n"
        "}\n"
        "uint yz_load_u8(uint loc, uint addr) {\n"
        "    return (yz_load_word(loc, addr & ~3u) >>\n"
        "            ((addr & 3u) * 8u)) & 0xFFu;\n"
        "}\n"
        /* f16 -> f32 with the CPU decoder's exact rule (subnormal halves
         * flush to signed zero; see rsx_compact_be_f16). */
        "float yz_half(uint h) {\n"
        "    uint s = (h >> 15) << 31;\n"
        "    uint e = (h >> 10) & 0x1Fu;\n"
        "    uint m = h & 0x3FFu;\n"
        "    uint bits = (e == 0u) ? s\n"
        "        : ((e == 31u) ? (s | 0x7F800000u | (m << 13))\n"
        "                      : (s | ((e + 112u) << 23) | (m << 13)));\n"
        "    return asfloat(bits);\n"
        "}\n"
        "float yz_snorm16(uint v) {\n"
        "    int sv = (int)(v << 16) >> 16;\n"
        "    return (float)sv / 32767.0;\n"
        "}\n"
        "float yz_sint16(uint v) {\n"
        "    int sv = (int)(v << 16) >> 16;\n"
        "    return (float)sv;\n"
        "}\n"
        /* rsx_vertex_element_index(): freq<=1 ordinary fetch in the 20-bit
         * domain; divider-op bit set = modulo, clear = division. */
        "uint yz_elem_index(uint vid, uint frequency, uint modulo) {\n"
        "    if (frequency <= 1u) return (vid + yz_base_index) & 0xFFFFFu;\n"
        "    if (modulo != 0u) return vid % frequency;\n"
        "    return vid / frequency;\n"
        "}\n"
        /* Resolve the guest vertex reference for this shader invocation:
         * ARRAYS passes the slot through (a host-built raw-index buffer
         * also lands here via SV_VertexID); INDEX_U16/U32 fetch the
         * big-endian guest index array in-shader. */
        "uint yz_vertex_ref(uint sysvid) {\n"
        "    uint slot = yz_first + sysvid;\n"
        "    if (yz_source == 1u)\n"
        "        return yz_load_be16(yz_pull_misc1.y,\n"
        "                            yz_pull_misc1.x + slot * 2u);\n"
        "    if (yz_source == 2u)\n"
        "        return yz_load_be32(yz_pull_misc1.y,\n"
        "                            yz_pull_misc1.x + slot * 4u);\n"
        "    return slot;\n"
        "}\n");

    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        const rsx_vertex_pull_attr* a = &plan->attr[attr];
        if (!a->pulled)
            continue;
        pull_emitf(&o,
            "float4 yz_pull_attr%u(uint vid) {\n"
            "    uint4 cfg = yz_attr_cfg[%u];\n"
            "    uint loc = cfg.w & 1u;\n"
            "    uint elem = yz_elem_index(vid, cfg.z, cfg.w & 2u);\n"
            "    uint addr = yz_base_offset + cfg.x + elem * cfg.y;\n"
            "    uint limit = (loc != 0u) ? yz_pull_misc1.w"
            " : yz_pull_misc1.z;\n"
            "    if (addr > limit || (limit - addr) < %uu)\n"
            "        return yz_attr_default[%u];\n"
            "    float4 value = float4(0.0, 0.0, 0.0, 1.0);\n",
            attr, attr, a->elem_size, attr);
        if (a->desc.type == RSX_VTX_TYPE_CMP32) {
            pull_emit(&o,
                "    uint w = yz_load_be32(loc, addr);\n"
                "    int cx = (int)(w & 0x7FFu);\n"
                "    if ((cx & 0x400) != 0) cx -= 0x800;\n"
                "    int cy = (int)((w >> 11) & 0x7FFu);\n"
                "    if ((cy & 0x400) != 0) cy -= 0x800;\n"
                "    int cz = (int)((w >> 22) & 0x3FFu);\n"
                "    if ((cz & 0x200) != 0) cz -= 0x400;\n"
                "    value = float4((float)cx / 1023.0,\n"
                "                   (float)cy / 1023.0,\n"
                "                   (float)cz / 511.0, 1.0);\n");
        } else {
            for (u32 c = 0; c < a->desc.size && c < 4u; c++)
                pull_emit_component(&o, a->desc.type, c);
        }
        pull_emit(&o, "    return value;\n}\n");
    }
    if (o.overflow)
        return -1;
    return (int)o.len;
}

int rsx_vertex_pull_emit_loads(const rsx_vertex_pull_plan* plan,
                               const char* vid_expr, char* out,
                               u32 out_size)
{
    if (!plan || !vid_expr || !out || out_size < 64)
        return -1;
    pull_out o = {out, out_size, 0, 0};
    out[0] = '\0';
    pull_emitf(&o, "    uint yz_vid = yz_vertex_ref(%s);\n", vid_expr);
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->layout.mask & (1u << attr)))
            continue;
        if (plan->attr[attr].pulled)
            pull_emitf(&o, "    v[%u] = yz_pull_attr%u(yz_vid);\n",
                       attr, attr);
        else
            pull_emitf(&o, "    v[%u] = yz_attr_default[%u];\n",
                       attr, attr);
    }
    if (o.overflow)
        return -1;
    return (int)o.len;
}

void rsx_vertex_pull_fill_constants(const rsx_vertex_pull_plan* plan,
                                    u32 base_index, u32 first, u32 source,
                                    u32 index_offset, u32 index_location,
                                    u32 mem_size_local, u32 mem_size_main,
                                    rsx_vertex_pull_constants* out)
{
    if (!plan || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->base_offset = plan->base_offset;
    out->base_index = base_index;
    out->first = first;
    out->source = source;
    out->index_offset = index_offset;
    out->index_location = index_location;
    out->mem_size[0] = mem_size_local;
    out->mem_size[1] = mem_size_main;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        const rsx_vertex_pull_attr* a = &plan->attr[attr];
        out->attr_cfg[attr][0] = a->desc.offset;
        out->attr_cfg[attr][1] = a->stride;
        out->attr_cfg[attr][2] = a->desc.frequency;
        out->attr_cfg[attr][3] =
            (a->desc.location ? RSX_PULL_ATTR_FLAG_MAIN : 0u) |
            (((plan->divider_mask >> attr) & 1u) ? RSX_PULL_ATTR_FLAG_MODULO
                                                 : 0u);
        memcpy(out->attr_default[attr], a->default_value, 16);
    }
}

int rsx_vertex_pull_decompile(const rsx_vertex_pull_plan* plan,
                              const u8* ucode, u32 max_bytes, u32 vtex_mask,
                              char* out, u32 out_size)
{
    return rsx_vertex_pull_decompile_control(
        plan, ucode, max_bytes, vtex_mask, 0u, out, out_size);
}

int rsx_vertex_pull_decompile_control(
    const rsx_vertex_pull_plan* plan, const u8* ucode, u32 max_bytes,
    u32 vtex_mask, u32 start_slot, char* out, u32 out_size)
{
    return rsx_vertex_pull_decompile_control_options(
        plan, ucode, max_bytes, vtex_mask, start_slot, 0u, out, out_size);
}

int rsx_vertex_pull_decompile_control_options(
    const rsx_vertex_pull_plan* plan, const u8* ucode, u32 max_bytes,
    u32 vtex_mask, u32 start_slot, u32 options, char* out, u32 out_size)
{
    /* Single render-thread shader-build path, same convention as the
     * decompiler's static body buffer. */
    static char globals[48 * 1024];
    static char loads[4 * 1024];
    if (rsx_vertex_pull_emit_globals(plan, globals, sizeof(globals)) < 0)
        return -1;
    if (rsx_vertex_pull_emit_loads(plan, "yz_sysvid", loads,
                                   sizeof(loads)) < 0)
        return -1;
    return rsx_vp_decompile_pull_control_options_ex(
        ucode, max_bytes, vtex_mask, start_slot, options,
        globals, loads, out, out_size);
}

int rsx_vertex_pull_flag_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("YZ_RSX_GPU_PULL");
        cached = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}
