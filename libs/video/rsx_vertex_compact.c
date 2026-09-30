#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define RSX_COMPACT_SSE2 1
#endif

static int rsx_compact_simd_unorm8;
static int rsx_compact_reuse_ranges;
void rsx_vertex_fetch_set_reuse_ranges(int enabled)
{
    rsx_compact_reuse_ranges = enabled != 0;
}
void rsx_vertex_decode_set_simd_unorm8(int enabled)
{
    rsx_compact_simd_unorm8 = enabled != 0;
}

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const u32 components = size < 4u ? size : 4u;
    /* Format is constant for the entire element. Dispatch once rather than
     * repeating the format switch for every component of every attribute. */
    if (!components)
        return 1;
    switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            for (u32 component = 0; component < components; component++)
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            for (u32 component = 0; component < components; component++)
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
#if defined(RSX_COMPACT_SSE2)
            if (rsx_compact_simd_unorm8 && components == 4) {
                const __m128 values = _mm_set_ps(
                    (float)source[3], (float)source[2],
                    (float)source[1], (float)source[0]);
                _mm_storeu_ps(out, _mm_div_ps(values, _mm_set1_ps(255.0f)));
                return 1;
            }
#endif
            for (u32 component = 0; component < components; component++)
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            for (u32 component = 0; component < components; component++)
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            for (u32 component = 0; component < components; component++) {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            }
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            for (u32 component = 0; component < components; component++) {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            }
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

u32 rsx_vertex_triangle_list_finalize(
    u32* indices, u32 count, int restart_enable, u32 restart_index)
{
    if (!indices && count)
        return 0u;
    u32 write = 0u;
    for (u32 read = 0u; read < count; ++read) {
        const u32 value = indices[read];
        if (restart_enable && value == restart_index)
            continue;
        indices[write++] = value;
    }
    return write - write % 3u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

void rsx_vertex_fetch_plan_init_decoded(
    rsx_vertex_fetch_plan* plan, const rsx_dsp_vertex_attr* attrs,
    const float defaults[][4], u32 base_offset, u32 divider_mask,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !attrs || !defaults || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = base_offset;
    plan->divider_mask = divider_mask;
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; ++attr) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->desc = attrs[attr];
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        memcpy(fetch->default_value, defaults[attr], 16u);
        if (attr == 3u &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

static void rsx_vertex_fetch_plan_prepare_ranges(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    struct range_entry { u32 frequency, modulo, first, last; } ranges[RSX_DSP_NUM_VERTEX_ATTR];
    u32 range_count = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !plan->guest_ptr ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        const u32 frequency = fetch->desc.frequency <= 1 ? 0 : fetch->desc.frequency;
        const u32 modulo = frequency ? (plan->divider_mask >> attr) & 1u : 0;
        u32 range = 0;
        while (range < range_count &&
               (ranges[range].frequency != frequency || ranges[range].modulo != modulo))
            ++range;
        if (range == range_count) {
            u32 first = UINT_MAX, last = 0;
            for (u32 i = 0; i < count; ++i) {
                const u32 element = rsx_vertex_element_index(
                    refs[i].vertex_id, refs[i].base_index, frequency, modulo);
                if (element < first) first = element;
                if (element > last) last = element;
            }
            ranges[range].frequency = frequency;
            ranges[range].modulo = modulo;
            ranges[range].first = first;
            ranges[range].last = last;
            ++range_count;
        }
        const u32 first = ranges[range].first, last = ranges[range].last;

        const u64 prefix =
            (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (rsx_compact_reuse_ranges) {
        rsx_vertex_fetch_plan_prepare_ranges(plan, refs, count);
        return;
    }
    if (!plan)
        return;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !plan->guest_ptr ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix =
            (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        float* value = (float*)(out + slot * 16u);
        if (!fetch->desc.type || !fetch->desc.size) {
            memcpy(value, fetch->default_value, 16);
            continue;
        }

        const u32 element = rsx_vertex_element_index(
            ref->vertex_id, ref->base_index, fetch->desc.frequency,
            (plan->divider_mask >> attr) & 1u);
        const u8* source = NULL;
        if (fetch->stable_base &&
            element >= fetch->stable_first && element <= fetch->stable_last) {
            source = fetch->stable_base +
                (u64)(element - fetch->stable_first) * fetch->stride;
        } else if (plan->guest_ptr) {
            const u32 offset =
                plan->base_offset + fetch->desc.offset +
                element * fetch->stride;
            source = plan->guest_ptr(
                plan->guest_user, fetch->desc.location, offset,
                fetch->elem_size);
        }

        if (source && rsx_vertex_decode_element(
                fetch->desc.type, fetch->desc.size, source, value))
            continue;
        if (attr == 0)
            return 0;
        memcpy(value, fetch->default_value, 16);
    }
    return 1;
}

/* Common stable-span formats: decide the format once per attribute. */
static u64 rsx_batch_fast_calls, rsx_batch_fallback_calls;
void rsx_vertex_fetch_batch_stats(u64* fast, u64* fallback)
{
    if (fast) *fast = rsx_batch_fast_calls;
    if (fallback) *fallback = rsx_batch_fallback_calls;
}

static int rsx_vertex_batch_reference(const rsx_vertex_fetch_plan* plan,
                           const rsx_vertex_ref* refs,u32 count,u8* out)
{
    if(!plan || (!refs && count) || (!out && count && plan->layout.stride)) return 0;
    for(u32 i=0;i<count;i++)
        if(!rsx_vertex_fetch_one(plan,&refs[i],out ? out+(u64)i*plan->layout.stride : NULL)) return 0;
    return 1;
}
int rsx_vertex_fetch_batch(const rsx_vertex_fetch_plan* plan,
                           const rsx_vertex_ref* refs,u32 count,u8* out)
{
    if(!plan || (!refs && count) || (!out && count && plan->layout.stride)) return 0;
    if(!count) return 1;
    u32 first=UINT_MAX,last=0;
    for(u32 i=0;i<count;i++) {
        u32 e=rsx_vertex_element_index(refs[i].vertex_id,refs[i].base_index,1,0);
        if(e<first) first=e; if(e>last) last=e;
    }
    for(u32 slot=0;slot<plan->layout.count;slot++) {
        const rsx_vertex_fetch_attr* f=&plan->attr[plan->layout.attrs[slot]];
        if(!f->desc.type || !f->desc.size) continue;
        const u32 n=f->desc.size<4 ? f->desc.size : 4;
        const int supported=(f->desc.type==RSX_VTX_TYPE_FLOAT && n==3) ||
            (f->desc.type==RSX_VTX_TYPE_HALF && n==2) ||
            ((f->desc.type==RSX_VTX_TYPE_UNORM8 || f->desc.type==RSX_VTX_TYPE_UINT8) && n==4);
        if(!supported || f->desc.frequency>1 || !f->stable_base ||
           first<f->stable_first || last>f->stable_last) {
            rsx_batch_fallback_calls++;
            return rsx_vertex_batch_reference(plan,refs,count,out);
        }
    }
    rsx_batch_fast_calls++;
    /* All source spans are proven and no resolver callbacks occur. Transposing
     * these loops permits one format dispatch per draw attribute. */
    for(u32 slot=0;slot<plan->layout.count;slot++) {
        const rsx_vertex_fetch_attr* f=&plan->attr[plan->layout.attrs[slot]];
        if(!f->desc.type || !f->desc.size) {
            for(u32 i=0;i<count;i++) memcpy(out+(u64)i*plan->layout.stride+slot*16u,f->default_value,16);
            continue;
        }
#define BATCH_LOOP(BODY) for(u32 i=0;i<count;i++) { \
        const u32 e=rsx_vertex_element_index(refs[i].vertex_id,refs[i].base_index,1,0); \
        const u8* source=f->stable_base+(u64)(e-f->stable_first)*f->stride; \
        float* value=(float*)(out+(u64)i*plan->layout.stride+slot*16u); \
        BODY; }
        switch(f->desc.type) {
        case RSX_VTX_TYPE_FLOAT:
            BATCH_LOOP(value[0]=rsx_compact_be_f32(source); value[1]=rsx_compact_be_f32(source+4); value[2]=rsx_compact_be_f32(source+8); value[3]=1.0f); break;
        case RSX_VTX_TYPE_HALF:
            BATCH_LOOP(value[0]=rsx_compact_be_f16(source); value[1]=rsx_compact_be_f16(source+2); value[2]=0.0f; value[3]=1.0f); break;
        case RSX_VTX_TYPE_UNORM8:
            BATCH_LOOP(value[0]=source[0]/255.0f; value[1]=source[1]/255.0f; value[2]=source[2]/255.0f; value[3]=source[3]/255.0f); break;
        case RSX_VTX_TYPE_UINT8:
            BATCH_LOOP(value[0]=(float)source[0]; value[1]=(float)source[1]; value[2]=(float)source[2]; value[3]=(float)source[3]); break;
        }
#undef BATCH_LOOP
    }
    return 1;
}

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
    }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
