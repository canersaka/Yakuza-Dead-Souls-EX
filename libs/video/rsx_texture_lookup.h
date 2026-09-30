#ifndef PS3RECOMP_RSX_TEXTURE_LOOKUP_H
#define PS3RECOMP_RSX_TEXTURE_LOOKUP_H

#include "ps3emu/ps3types.h"
#include <string.h>

#define RSX_TEXTURE_LOOKUP_CAPACITY 1024u
#define RSX_TEXTURE_LOOKUP_BUCKETS 2048u
#define RSX_TEXTURE_LOOKUP_MISSING (~0u)

/* A descriptor index only: content refresh, resources and LRU remain owned
 * by the renderer. Hash collisions always compare the entire exact key. */
typedef struct rsx_texture_key {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
} rsx_texture_key;
typedef struct rsx_texture_lookup {
    u32 heads[RSX_TEXTURE_LOOKUP_BUCKETS];
    u32 next[RSX_TEXTURE_LOOKUP_CAPACITY];
    u32 hashes[RSX_TEXTURE_LOOKUP_CAPACITY];
    u8 present[RSX_TEXTURE_LOOKUP_CAPACITY];
    rsx_texture_key keys[RSX_TEXTURE_LOOKUP_CAPACITY];
} rsx_texture_lookup;

static inline u32 rsx_texture_key_hash(const rsx_texture_key* key)
{
    const u32 words[] = {key->location, key->offset, key->format,
        key->width, key->height, key->pitch, key->remap, key->cubemap};
    u32 hash = 2166136261u;
    for (u32 i = 0; i < 8; ++i) hash = (hash ^ words[i]) * 16777619u;
    /* Mix high address bits into the bucket for aligned texture allocations. */
    hash ^= hash >> 16; hash *= 0x85EBCA6Bu;
    hash ^= hash >> 13; hash *= 0xC2B2AE35u;
    return hash ^ (hash >> 16);
}
static inline u32 rsx_texture_lookup_find(const rsx_texture_lookup* table,
                                         const rsx_texture_key* key)
{
    const u32 hash = rsx_texture_key_hash(key);
    u32 result = RSX_TEXTURE_LOOKUP_MISSING;
    for (u32 link = table->heads[hash & (RSX_TEXTURE_LOOKUP_BUCKETS-1u)];
         link; link = table->next[link-1u]) {
        const u32 index = link-1u;
        if (table->hashes[index] == hash && index < result &&
            !memcmp(&table->keys[index], key, sizeof(*key))) result = index;
    }
    /* Return the lowest matching slot so duplicate keys resolve deterministically. */
    return result;
}
static inline void rsx_texture_lookup_set(rsx_texture_lookup* table, u32 index,
                                         const rsx_texture_key* key)
{
    if (index >= RSX_TEXTURE_LOOKUP_CAPACITY) return;
    if (table->present[index]) {
        u32* link = &table->heads[table->hashes[index] & (RSX_TEXTURE_LOOKUP_BUCKETS-1u)];
        while (*link && *link != index+1u) link = &table->next[*link-1u];
        if (*link) *link = table->next[index];
    }
    const u32 hash = rsx_texture_key_hash(key);
    u32* head = &table->heads[hash & (RSX_TEXTURE_LOOKUP_BUCKETS-1u)];
    table->keys[index] = *key;
    table->hashes[index] = hash;
    table->present[index] = 1;
    table->next[index] = *head;
    *head = index+1u;
}
#endif
