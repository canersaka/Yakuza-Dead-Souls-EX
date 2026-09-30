/* Exact-byte metadata reuse for serialized renderer calls. Entries own bytes
 * through the first END instruction. Every hit validates the available length
 * and all stored bytes, so uploads, changed terminators and pointer reuse cannot
 * retain stale metadata. Invalid/unterminated programs are never cached. */
#ifndef RSX_VP_METADATA_CACHE_H
#define RSX_VP_METADATA_CACHE_H
#include "rsx_dispatch.h"
#include "rsx_vp_decompiler.h"
#include "rsx_hash.h"
#include <string.h>

typedef struct {
    u32 instructions, input_mask, txl_mask;
    u64 hash;
} vp_metadata;
typedef struct {
    vp_metadata metadata;
    u8 bytes[RSX_DSP_VP_INSTR * 16];
} vp_metadata_entry;
typedef struct {
    vp_metadata_entry entries[32];
    u32 next, recent;
} vp_metadata_cache;

static vp_metadata vp_metadata_reference(const u8* code, u32 available)
{
    vp_metadata value = {0};
    value.instructions = rsx_vp_program_size_instrs(code, available);
    if (!value.instructions) return value;
    rsx_vp_input_analysis analysis = {0xFFFFu, 0};
    if (rsx_vp_analyze_inputs(code, value.instructions * 16, &analysis) !=
            (int)value.instructions || !analysis.exact)
        analysis.input_mask = 0xFFFFu;
    value.input_mask = analysis.input_mask;
    value.hash = rsx_fnv1a(code, value.instructions * 16, RSX_FNV64_OFFSET);
    for (u32 i = 0; i < value.instructions; ++i) {
        const u8* p = code + i * 16;
        const u32 d1 = (u32)p[4] | ((u32)p[5] << 8) |
                       ((u32)p[6] << 16) | ((u32)p[7] << 24);
        const u32 d2 = (u32)p[8] | ((u32)p[9] << 8) |
                       ((u32)p[10] << 16) | ((u32)p[11] << 24);
        if (((d1 >> 22) & 31u) == 0x19u)
            value.txl_mask |= 1u << ((d2 >> 8) & 3u);
    }
    return value;
}

static vp_metadata vp_metadata_lookup(vp_metadata_cache* cache,
                                      const u8* code, u32 available)
{
    if (!code || available < 16) return vp_metadata_reference(code, available);
    for (u32 step = 0; step < 32; ++step) {
        const u32 index = (cache->recent + step) & 31u;
        const vp_metadata_entry* entry = &cache->entries[index];
        const u32 size = entry->metadata.instructions * 16;
        if (size && size <= available && !memcmp(entry->bytes, code, size)) {
            cache->recent = index;
            return entry->metadata;
        }
    }
    const vp_metadata value = vp_metadata_reference(code, available);
    if (value.instructions && value.instructions <= RSX_DSP_VP_INSTR) {
        const u32 index = cache->next++ & 31u;
        cache->entries[index].metadata = value;
        memcpy(cache->entries[index].bytes, code, value.instructions * 16);
        cache->recent = index;
    }
    return value;
}
#endif
