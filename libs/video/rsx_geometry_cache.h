/* Exact-input, immutable-output geometry reuse. Metadata selects candidates;
 * all source bytes are compared on a hit. On a miss the fetch plan is rebound
 * to owned snapshots so cached output cannot be paired with different input. */
#define LD_GEOMETRY_CACHE_BUCKETS 512u
#define LD_GEOMETRY_CACHE_WAYS 8u
#define LD_GEOMETRY_KEY_LIMIT (4u*1024u*1024u)
#define LD_GEOMETRY_INPUT_LIMIT (256ull*1024u*1024u)
#define LD_GEOMETRY_GPU_LIMIT (512ull*1024u*1024u)
typedef struct ld_geometry_buffer {
    ID3D12Resource* resource;
    void* mapped;
    u64 capacity, retire_fence, resource_offset;
    struct ld_geometry_buffer* next;
} ld_geometry_buffer;
static ld_geometry_buffer* g_geometry_free[33];
static u8* g_geometry_index_scratch;
static u64 g_geometry_index_capacity;
typedef struct ld_geometry_entry {
    u64 hash, age, generation;
    u32 key_size, input_size, input_offset[16], input_bytes[16];
    u64 vb_bytes, ib_offset, ib_bytes;
    u32 topology_count, indexed;
    u8* input;
    ID3D12Resource* resource;
    ld_geometry_buffer* buffer;
    int valid;
} ld_geometry_entry;
static struct {
    int enabled;
    u8* key;
    u32 key_size, key_capacity;
    u64 age, hits, misses, bypasses, failures, compared_bytes;
    u64 input_bytes, gpu_bytes;
    ld_geometry_entry entries[LD_GEOMETRY_CACHE_BUCKETS*LD_GEOMETRY_CACHE_WAYS];
} g_geometry_cache;
static ld_geometry_entry* g_draw_geometry_cache;
static int g_draw_geometry_cache_hit;
#include "rsx_geometry_arena.h"
static u64 g_geometry_entry_generation;
static void* ld_geometry_index_destination(void* fallback, u64 bytes)
{
    if (!g_draw_geometry_cache || g_draw_geometry_cache_hit || !bytes) return fallback;
    if (bytes > g_geometry_index_capacity) {
        u8* memory = realloc(g_geometry_index_scratch, (size_t)bytes);
        if (!memory) return fallback;
        g_geometry_index_scratch = memory; g_geometry_index_capacity = bytes;
    }
    return g_geometry_index_scratch;
}

int rsx_live_draw_set_geometry_cache(int enabled)
{
    g_geometry_cache.enabled = enabled != 0;
    return g_geometry_cache.enabled;
}
static int ld_geometry_key_append(const void* data, u32 bytes)
{
    if (bytes > LD_GEOMETRY_KEY_LIMIT-g_geometry_cache.key_size) return 0;
    u32 required = g_geometry_cache.key_size+bytes;
    if (required > g_geometry_cache.key_capacity) {
        u32 capacity = g_geometry_cache.key_capacity ? g_geometry_cache.key_capacity : 4096u;
        while (capacity < required) capacity *= 2u;
        u8* key = realloc(g_geometry_cache.key, capacity);
        if (!key) return 0;
        g_geometry_cache.key = key; g_geometry_cache.key_capacity = capacity;
    }
    if (bytes) memcpy(g_geometry_cache.key+g_geometry_cache.key_size, data, bytes);
    g_geometry_cache.key_size = required;
    return 1;
}
static void ld_geometry_entry_evict(ld_geometry_entry* e)
{
    if (e->buffer) {
        u32 bin = 0; u64 size = e->buffer->capacity;
        while (size > 1) { ++bin; size >>= 1; }
        /* Include references still in the currently recording command list. */
        e->buffer->retire_fence = g.fence_value+1;
        e->buffer->next = g_geometry_free[bin];
        g_geometry_free[bin] = e->buffer;
    }
    g_geometry_cache.input_bytes -= e->input_size;
    free(e->input);
    memset(e, 0, sizeof(*e));
}
/* The mesh record supplies this immutable key and prepared attribute ranges.
 * A known changed version can replace its slot without hashing references or
 * comparing the same failed payload twice. All replacements retain the normal
 * owned snapshot and fence retirement contract. */
static int ld_geometry_cache_prepare_record(rsx_vertex_fetch_plan* plan,
    const u8* key, u32 key_size, u64 hash, ld_geometry_entry* changed)
{
    u32 spans[16] = {0};
    u64 source_bytes = 0;
    for (u32 slot = 0; slot < plan->layout.count; ++slot) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* a = &plan->attr[attr];
        if (!a->desc.type || !a->desc.size) continue;
        if (!a->stable_base || a->stable_last < a->stable_first) goto bypass;
        const u64 bytes = (u64)(a->stable_last-a->stable_first)*a->stride+a->elem_size;
        if (bytes > LD_GEOMETRY_KEY_LIMIT || source_bytes+bytes > LD_GEOMETRY_KEY_LIMIT) goto bypass;
        spans[attr] = (u32)bytes; source_bytes += bytes;
    }
    const u32 bucket = (u32)(hash ^ (hash >> 32)) & (LD_GEOMETRY_CACHE_BUCKETS-1u);
    ld_geometry_entry* selected = changed;
    for (u32 way = 0; !changed && way < LD_GEOMETRY_CACHE_WAYS; ++way) {
        ld_geometry_entry* e = &g_geometry_cache.entries[bucket*LD_GEOMETRY_CACHE_WAYS+way];
        if (!selected || (!e->valid && selected->valid) || (e->valid == selected->valid && e->age < selected->age)) selected = e;
        if (!e->valid || e->hash != hash || e->key_size != key_size ||
            memcmp(e->input, key, e->key_size)) continue;
        int equal = 1;
        for (u32 attr = 0; attr < 16; ++attr) if (spans[attr]) {
            g_geometry_cache.compared_bytes += spans[attr];
            if (e->input_bytes[attr] != spans[attr] ||
                memcmp(e->input+e->input_offset[attr], plan->attr[attr].stable_base, spans[attr])) {
                equal = 0; break;
            }
        }
        if (equal) {
            ++g_geometry_cache.hits; e->age = ++g_geometry_cache.age;
            g_draw_geometry_cache = e; g_draw_geometry_cache_hit = 1;
            return 1;
        }
        /* Replace this same structural key's old content version on a miss. */
        selected = e;
        break;
    }
    if (!selected || source_bytes+key_size > LD_GEOMETRY_KEY_LIMIT ||
        g_geometry_cache.input_bytes-selected->input_size+source_bytes+key_size > LD_GEOMETRY_INPUT_LIMIT) goto bypass;
    const u32 bytes = key_size+(u32)source_bytes;
    u8* snapshot = malloc(bytes ? bytes : 1u);
    if (!snapshot) goto bypass;
    memcpy(snapshot, key, key_size);
    ld_geometry_entry_evict(selected);
    selected->input = snapshot; selected->input_size = bytes;
    selected->key_size = key_size; selected->hash = hash;
    selected->generation = ++g_geometry_entry_generation;
    selected->age = ++g_geometry_cache.age;
    u32 offset = selected->key_size;
    for (u32 attr = 0; attr < 16; ++attr) if (spans[attr]) {
        selected->input_offset[attr] = offset; selected->input_bytes[attr] = spans[attr];
        memcpy(snapshot+offset, plan->attr[attr].stable_base, spans[attr]);
        plan->attr[attr].stable_base = snapshot+offset;
        offset += spans[attr];
    }
    g_geometry_cache.input_bytes += bytes;
    ++g_geometry_cache.misses;
    g_draw_geometry_cache = selected;
    return 0;
bypass:
    ++g_geometry_cache.bypasses;
    return 0;
}
static int ld_geometry_cache_prepare_impl(rsx_vertex_fetch_plan* plan)
{
    if (!g_geometry_cache.enabled) return 0;
    g_geometry_cache.key_size = 0;
#define LD_GEOMETRY_KEY(data, bytes) do { if (!ld_geometry_key_append((data), (u32)(bytes))) goto bypass; } while (0)
    const u32 shape[] = {g.rsx.current_primitive, plan->base_offset, plan->divider_mask,
        dc.n_refs, dc.n_source_refs, dc.n_cuts, (u32)dc.refs_remapped, dc.n_arr, dc.n_idx};
    LD_GEOMETRY_KEY(shape, sizeof(shape));
    LD_GEOMETRY_KEY(&plan->layout, sizeof(plan->layout));
    if (dc.n_refs > LD_GEOMETRY_KEY_LIMIT/sizeof(*dc.refs)) goto bypass;
    LD_GEOMETRY_KEY(dc.refs, dc.n_refs*sizeof(*dc.refs));
    LD_GEOMETRY_KEY(dc.cuts, dc.n_cuts*sizeof(*dc.cuts));
    LD_GEOMETRY_KEY(dc.arr, dc.n_arr*sizeof(*dc.arr));
    LD_GEOMETRY_KEY(dc.idx, dc.n_idx*sizeof(*dc.idx));
    if (dc.refs_remapped) {
        if (dc.n_source_refs > LD_GEOMETRY_KEY_LIMIT/sizeof(u32)) goto bypass;
        LD_GEOMETRY_KEY(dc.ref_remap.occurrence_to_unique, dc.n_source_refs*sizeof(u32));
    }
    for (u32 slot = 0; slot < plan->layout.count; ++slot) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* a = &plan->attr[attr];
        LD_GEOMETRY_KEY(&a->desc, sizeof(a->desc));
        LD_GEOMETRY_KEY(a->default_value, sizeof(a->default_value));
        const u32 range[] = {a->elem_size, a->stride, a->stable_first, a->stable_last};
        LD_GEOMETRY_KEY(range, sizeof(range));

    }
#undef LD_GEOMETRY_KEY
    const u64 hash = rsx_fnv1a_words(g_geometry_cache.key, g_geometry_cache.key_size);
    return ld_geometry_cache_prepare_record(plan, g_geometry_cache.key,
        g_geometry_cache.key_size, hash, NULL);
bypass:
    ++g_geometry_cache.bypasses;
    return 0;
}
static void ld_geometry_cache_store_impl(const void* vertices, u64 vb_bytes,
                                     const void* indices, u64 ib_bytes)
{
    ld_geometry_entry* e = g_draw_geometry_cache;
    if (!e || g_draw_geometry_cache_hit) return;
    const u64 ib_offset = (vb_bytes+15u)&~15ull;
    const u64 bytes = ib_offset+ib_bytes;
    u64 capacity = 256; u32 bin = 8;
    while (capacity < bytes) { capacity *= 2; ++bin; }
    const u64 completed = g.fence->lpVtbl->GetCompletedValue(g.fence);
    /* Reuse retired slices before growing the arena. Each live slice retains
     * the same immutable bytes and fence lifetime as a committed buffer. */
    ld_geometry_buffer** link = &g_geometry_free[bin];
    while (*link && (*link)->retire_fence > completed) link = &(*link)->next;
    ld_geometry_buffer* buffer = *link;
    if (buffer) {
        *link = buffer->next;
        buffer->next = NULL;
        goto fill;
    }
    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) goto fail;
    {
        if (!ld_geometry_arena_allocate(capacity, buffer)) {
            free(buffer);
            goto fail;
        }
    }
fill:
    if (vb_bytes) memcpy(buffer->mapped, vertices, (size_t)vb_bytes);
    if (ib_bytes) memcpy((u8*)buffer->mapped+ib_offset, indices, (size_t)ib_bytes);
    e->buffer = buffer; e->resource = buffer->resource;
    e->vb_bytes = vb_bytes; e->ib_bytes = ib_bytes; e->ib_offset = ib_offset;
    e->valid = 1;
    return;
fail:
    ++g_geometry_cache.failures;
}
static int ld_geometry_cache_prepare(rsx_vertex_fetch_plan* plan)
{
    int result = ld_geometry_cache_prepare_impl(plan);
    return result;
}
static void ld_geometry_cache_store(const void* vertices, u64 vb_bytes,
                                     const void* indices, u64 ib_bytes, u32 topology_count, int indexed)
{
    if(!g_draw_geometry_cache || !g_draw_geometry_cache->valid)
        ld_geometry_cache_store_impl(vertices, vb_bytes, indices, ib_bytes);
    if(g_draw_geometry_cache && g_draw_geometry_cache->valid) {
        g_draw_geometry_cache->topology_count=topology_count;
        g_draw_geometry_cache->indexed=indexed!=0;
    }
}
/* Called only after the ordinary final GPU drain. */
static void ld_geometry_cache_destroy(void)
{
    for (u32 i = 0; i < LD_GEOMETRY_CACHE_BUCKETS*LD_GEOMETRY_CACHE_WAYS; ++i) {
        ld_geometry_entry* e = &g_geometry_cache.entries[i];
        if (e->resource) e->resource->lpVtbl->Release(e->resource);
        free(e->buffer);
        free(e->input);
    }
    for (u32 bin = 0; bin < 33; ++bin) {
        ld_geometry_buffer* buffer = g_geometry_free[bin];
        while (buffer) {
            ld_geometry_buffer* next = buffer->next;
            buffer->resource->lpVtbl->Release(buffer->resource);
            free(buffer); buffer = next;
        }
        g_geometry_free[bin] = NULL;
    }
    ld_geometry_arena_destroy();
    free(g_geometry_cache.key);
    free(g_geometry_index_scratch);
    g_geometry_index_scratch = NULL; g_geometry_index_capacity = 0;
    memset(&g_geometry_cache, 0, sizeof(g_geometry_cache));
    g_draw_geometry_cache = NULL;
    g_draw_geometry_cache_hit = 0;
}
void rsx_live_draw_dump_geometry_cache(void)
{
}
