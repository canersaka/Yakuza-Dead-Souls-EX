#ifndef RSX_HASH_H
#define RSX_HASH_H

/* 64-bit FNV-1a, shared by the renderer's caches. */

#include "ps3emu/ps3types.h"
#include <string.h>

#define RSX_FNV64_OFFSET 1469598103934665603ull
#define RSX_FNV64_PRIME  1099511628211ull

/* Byte-wise FNV-1a over data, continuing from hash. */
static inline u64 rsx_fnv1a(const void* data, u32 size, u64 hash)
{
    const u8* bytes = (const u8*)data;
    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= RSX_FNV64_PRIME;
    }
    return hash;
}

/* FNV-1a over 8-byte host words, then the remaining bytes. Cheaper for long
 * inputs; it does not produce the same value as rsx_fnv1a. */
static inline u64 rsx_fnv1a_words(const u8* data, u32 size)
{
    u64 hash = RSX_FNV64_OFFSET;
    u32 i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 word;
        memcpy(&word, data + i, sizeof(word));
        hash = (hash ^ word) * RSX_FNV64_PRIME;
    }
    for (; i < size; ++i)
        hash = (hash ^ data[i]) * RSX_FNV64_PRIME;
    return hash;
}

#endif
