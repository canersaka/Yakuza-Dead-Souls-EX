#ifndef RSX_MATERIAL_CACHE_H
#define RSX_MATERIAL_CACHE_H

#include "ps3emu/ps3types.h"
#include <string.h>

#define RSX_MATERIAL_UNITS 16u
#define RSX_MATERIAL_KEY_WORDS (16u + 16u * 8u + 1u)
#define RSX_MATERIAL_BUCKETS 1024u
#define RSX_MATERIAL_WAYS 4u

/* Exact register identity only. Resource versions, content freshness and
 * render-target aliasing must also be validated before a binding is used. */
typedef struct rsx_material_key { u32 words[RSX_MATERIAL_KEY_WORDS]; } rsx_material_key;
typedef struct rsx_material_record {
    rsx_material_key key;
    u64 hash, age, surface_epoch;
    u32 surface_count, texture_mask, dynamic_mask;
    u32 surfaces[RSX_MATERIAL_UNITS], num_surfaces;
    u32 slots[RSX_MATERIAL_UNITS], samplers[RSX_MATERIAL_UNITS];
    u64 versions[RSX_MATERIAL_UNITS];
    /* Descriptor copies belong to one live GPU heap generation. */
    u64 table_epoch, table_handle, sampler_handle;
} rsx_material_record;
typedef struct rsx_material_cache {
    u64 age;
    rsx_material_record entries[RSX_MATERIAL_BUCKETS * RSX_MATERIAL_WAYS];
} rsx_material_cache;

static inline u64 rsx_material_hash(const rsx_material_key* key)
{
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < RSX_MATERIAL_KEY_WORDS; ++i)
        hash = (hash ^ key->words[i]) * 1099511628211ull;
    return hash ^ (hash >> 32);
}
static inline rsx_material_record* rsx_material_find(
    rsx_material_cache* cache, const rsx_material_key* key, u64 hash)
{
    rsx_material_record* ways = cache->entries +
        (hash & (RSX_MATERIAL_BUCKETS - 1u)) * RSX_MATERIAL_WAYS;
    for (u32 i = 0; i < RSX_MATERIAL_WAYS; ++i)
        if (ways[i].age && ways[i].hash == hash &&
            !memcmp(&ways[i].key, key, sizeof(*key)))
            return &ways[i];
    return NULL;
}
static inline rsx_material_record* rsx_material_store(
    rsx_material_cache* cache, const rsx_material_key* key, u64 hash)
{
    rsx_material_record* record = rsx_material_find(cache, key, hash);
    if (!record) {
        rsx_material_record* ways = cache->entries +
            (hash & (RSX_MATERIAL_BUCKETS - 1u)) * RSX_MATERIAL_WAYS;
        record = ways;
        for (u32 i = 1; i < RSX_MATERIAL_WAYS; ++i)
            if (ways[i].age < record->age) record = &ways[i];
    }
    if (++cache->age == 0) {
        memset(cache, 0, sizeof(*cache));
        cache->age = 1;
    }
    memset(record, 0, sizeof(*record));
    record->key = *key;
    record->hash = hash;
    record->age = cache->age;
    return record;
}
static inline int rsx_material_versions_match(const rsx_material_record* record,
    u64 surface_epoch, u32 surface_count, const u64* versions, u32 count)
{
    if (!record || !record->age || record->surface_epoch != surface_epoch ||
        record->surface_count != surface_count) return 0;
    for (u32 u = 0; u < RSX_MATERIAL_UNITS; ++u)
        if (!(record->dynamic_mask & (1u << u)) && (record->slots[u] >= count ||
            versions[record->slots[u]] != record->versions[u])) return 0;
    return 1;
}
static inline u64 rsx_material_table_handle(const rsx_material_record* record,
                                           u64 epoch)
{
    return record && epoch && record->table_epoch == epoch
        ? record->table_handle : 0;
}
/* Dynamic depth resolution has already run. Reuse its descriptor copies only
 * while the resolved CPU slots AND their resource generations still match. */
static inline int rsx_material_update_dynamic(rsx_material_record* record,
    const u32 slots[RSX_MATERIAL_UNITS], const u64* versions, u32 count)
{
    if (!record) return 0;
    for (u32 u = 0; u < RSX_MATERIAL_UNITS; ++u)
        if ((record->dynamic_mask & (1u << u)) && slots[u] >= count) return 0;
    for (u32 u = 0; u < RSX_MATERIAL_UNITS; ++u) {
        if (!(record->dynamic_mask & (1u << u))) continue;
        const u64 version = versions[slots[u]];
        if (record->slots[u] != slots[u] || record->versions[u] != version)
            record->table_epoch = 0;
        record->slots[u] = slots[u];
        record->versions[u] = version;
    }
    return 1;
}
static inline void rsx_material_advance_epoch(rsx_material_cache* cache, u64* epoch)
{
    if (++*epoch == 0) {
        memset(cache, 0, sizeof(*cache));
        *epoch = 1;
    }
}
#endif
