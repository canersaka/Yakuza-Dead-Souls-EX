/* Prepared mesh lookup. Mesh records own immutable topology and GPU payload
 * versions independently of the shared geometry cache. Metadata selects a
 * candidate; index and vertex bytes are still compared byte for byte before
 * reuse. Only misses snapshot indices and decode them again. */
#define LD_EARLY_BUCKETS 512u
#define LD_EARLY_WAYS 8u
#define LD_EARLY_MEMORY_LIMIT (64ull*1024u*1024u)
#define LD_MESH_BUCKETS 2048u
typedef struct ld_early_entry {
    u64 hash, age, generation;
    u8* key;
    u32 key_size, refs, source_refs, cuts, remapped;
    rsx_vertex_fetch_plan plan;
    ld_geometry_entry* geometry;
    /* Topology survives vertex-content changes and geometry-cache eviction. */
    u8* mesh;
    u32 metadata_size, mesh_key_size, mesh_size, topology_count, indexed;
    u64 mesh_hash;
    int owns_geometry;
} ld_early_entry;
static struct {
    int configured, enabled, active, hit, meshes_configured, meshes, mesh_hit;
    u8* key;
    u32 size, capacity, offset_capacity, metadata_size;
    u32* index_offsets;
    u64 hash, age, memory;
    const u8* index_live[256];
    u32 index_bytes[256];
    ld_early_entry* current_mesh;
    ld_early_entry* meshes_entries;
    ld_early_entry entries[LD_EARLY_BUCKETS*LD_EARLY_WAYS];
} g_early_geometry;
static void ld_early_mesh_entries_clear(void)
{
    if (!g_early_geometry.meshes_entries) return;
    for (u32 i=0;i<LD_MESH_BUCKETS*LD_EARLY_WAYS;++i) {
        ld_early_entry* e=&g_early_geometry.meshes_entries[i];
        g_early_geometry.memory-=e->key_size+e->mesh_size;
        free(e->key); free(e->mesh);
        if (e->owns_geometry && g_draw_geometry_cache==e->geometry) {
            g_draw_geometry_cache=NULL; g_draw_geometry_cache_hit=0;
        }
        if (e->owns_geometry) { ld_geometry_entry_evict(e->geometry); free(e->geometry); }
        memset(e,0,sizeof(*e));
    }
    g_early_geometry.mesh_hit=0; g_early_geometry.current_mesh=NULL;
}
static int ld_set_prepared_meshes(int enabled)
{
    const int previous=g_early_geometry.meshes;
    g_early_geometry.meshes_configured = 1;
    g_early_geometry.meshes = enabled;
    if (g_early_geometry.meshes && !g_early_geometry.meshes_entries) {
        g_early_geometry.meshes_entries = calloc(LD_MESH_BUCKETS*LD_EARLY_WAYS, sizeof(ld_early_entry));
        if (!g_early_geometry.meshes_entries) g_early_geometry.meshes = 0;
    }
    /* Disabling the mesh table returns its buffers to the fence-aware pool. */
    if (previous && !g_early_geometry.meshes) ld_early_mesh_entries_clear();
    return g_early_geometry.meshes;
}

static void write_topology_indices_fast(u32 prim, u32* destination);

static u64 ld_early_hash(const u8* key, u32 size)
{
    return rsx_fnv1a_words(key, size);
}

int rsx_live_draw_set_early_geometry_cache(int enabled)
{
    g_early_geometry.configured = 1;
    g_early_geometry.enabled = !!enabled;
    return g_early_geometry.enabled;
}
static int ld_early_append(const void* data, u32 bytes)
{
    if (bytes > LD_GEOMETRY_KEY_LIMIT-g_early_geometry.size) return 0;
    u32 needed = g_early_geometry.size+bytes;
    if (needed > g_early_geometry.capacity) {
        u32 capacity = g_early_geometry.capacity ? g_early_geometry.capacity : 4096u;
        while (capacity < needed) capacity *= 2;
        u8* memory = realloc(g_early_geometry.key, capacity);
        if (!memory) return 0;
        g_early_geometry.key = memory; g_early_geometry.capacity = capacity;
    }
    if (bytes) memcpy(g_early_geometry.key+g_early_geometry.size, data, bytes);
    g_early_geometry.size = needed;
    return 1;
}
static int ld_early_reserve(void** data, u32* capacity, u32 count, size_t size)
{
    if (count <= *capacity) return 1;
    if (count > SIZE_MAX/size) return 0;
    void* memory = realloc(*data, (size_t)count*size);
    if (!memory) return 0;
    *data = memory; *capacity = count;
    return 1;
}
static int ld_early_restore(ld_early_entry* entry)
{
    ld_geometry_entry* geometry = entry->geometry;
    if (!geometry || !geometry->valid || geometry->generation != entry->generation)
        return 0;
    rsx_vertex_fetch_plan plan = entry->plan;
    plan.guest_ptr = (rsx_vertex_guest_ptr_fn)g.guest_ptr;
    plan.guest_user = g.guest_user;
    const u8* live[16] = {0};
    for (u32 slot = 0; slot < plan.layout.count; ++slot) {
        const u32 attr = plan.layout.attrs[slot];
        rsx_vertex_fetch_attr* a = &plan.attr[attr];
        const u32 bytes = geometry->input_bytes[attr];
        if (!a->desc.type || !a->desc.size) continue;
        if (!bytes || !plan.guest_ptr) return 0;
        const u64 start = (u64)plan.base_offset+a->desc.offset+
                          (u64)a->stable_first*a->stride;
        if (start > UINT32_MAX || start+bytes > 0x100000000ull) return 0;
        const u8* source = plan.guest_ptr(plan.guest_user, a->desc.location,
                                        (u32)start, bytes);
        if (!source) return 0;
        live[attr] = source;
        a->stable_base = source;
    }
    for (u32 slot = 0; slot < plan.layout.count; ++slot) {
        const u32 attr=plan.layout.attrs[slot];
        const u32 bytes=geometry->input_bytes[attr];
        const u8* source=live[attr];
        if (!source) continue;
        if (memcmp(source, geometry->input+geometry->input_offset[attr], bytes)) return 0;
        g_geometry_cache.compared_bytes += bytes;
    }
    /* The existing exact key owns refs, cuts and occurrence mapping. Restore
     * them as well as the output counts so downstream topology and diagnostic
     * paths retain the original draw representation. */
    const u64 refs_offset = 9u*sizeof(u32)+sizeof(rsx_vertex_layout_plan);
    const u64 cuts_offset = refs_offset+(u64)entry->refs*sizeof(rsx_vertex_ref);
    const u64 map_offset = cuts_offset+(u64)entry->cuts*sizeof(u32)+
        (u64)dc.n_arr*sizeof(*dc.arr)+(u64)dc.n_idx*sizeof(*dc.idx);
    const u64 end = map_offset+(entry->remapped ? (u64)entry->source_refs*sizeof(u32) : 0);
    if (end > geometry->key_size) return 0;
    if (!g_early_geometry.meshes) {
    if (!ld_early_reserve((void**)&dc.refs, &dc.cap_refs, entry->refs, sizeof(*dc.refs)) ||
        !ld_early_reserve((void**)&dc.cuts, &dc.cap_cuts, entry->cuts, sizeof(*dc.cuts)) ||
        (entry->remapped && !ld_early_reserve((void**)&dc.ref_remap.occurrence_to_unique,
             &dc.ref_remap.occurrence_capacity, entry->source_refs, sizeof(u32))))
        return 0;
    if (entry->refs) memcpy(dc.refs, geometry->input+refs_offset, (size_t)entry->refs*sizeof(*dc.refs));
    if (entry->cuts) memcpy(dc.cuts, geometry->input+cuts_offset, (size_t)entry->cuts*sizeof(*dc.cuts));
    if (entry->remapped) memcpy(dc.ref_remap.occurrence_to_unique, geometry->input+map_offset,
                               (size_t)entry->source_refs*sizeof(u32));
    }
    dc.n_refs = dc.n_verts = entry->refs;
    dc.n_source_refs = entry->source_refs; dc.n_cuts = entry->cuts;
    dc.refs_remapped = entry->remapped;
    dc.layout = plan.layout; dc.fetch_plan = plan;
    g_draw_geometry_cache = geometry; g_draw_geometry_cache_hit = 1;
    geometry->age = ++g_geometry_cache.age;
    ++g_geometry_cache.hits;
    return 1;
}

/* Restore the independent mesh interpretation after a vertex-only change.
 * Resolve every live range again: a cached host pointer is not a mapping token.
 * The record owns refs/cuts/remap and the full key even if its GPU version was
 * evicted. No index scan, deduplication, range discovery or topology expansion
 * is repeated. The normal geometry cache owns each changed vertex snapshot. */
static int ld_early_restore_mesh(ld_early_entry* e)
{
    if (!e->mesh || !e->mesh_key_size) return 0;
    rsx_vertex_fetch_plan plan = e->plan;
    plan.guest_ptr = (rsx_vertex_guest_ptr_fn)g.guest_ptr;
    plan.guest_user = g.guest_user;
    for (u32 slot = 0; slot < plan.layout.count; ++slot) {
        rsx_vertex_fetch_attr* a = &plan.attr[plan.layout.attrs[slot]];
        if (!a->desc.type || !a->desc.size) continue;
        const u64 bytes = (u64)(a->stable_last-a->stable_first)*a->stride+a->elem_size;
        const u64 start = (u64)plan.base_offset+a->desc.offset+(u64)a->stable_first*a->stride;
        if (!plan.guest_ptr || a->stable_last < a->stable_first ||
            !bytes || bytes > LD_GEOMETRY_KEY_LIMIT || start > UINT32_MAX ||
            start+bytes > 0x100000000ull) return 0;
        a->stable_base = plan.guest_ptr(plan.guest_user, a->desc.location, (u32)start, (u32)bytes);
        if (!a->stable_base) return 0;
    }
    const u64 refs_offset = 9u*sizeof(u32)+sizeof(rsx_vertex_layout_plan);
    const u64 cuts_offset = refs_offset+(u64)e->refs*sizeof(rsx_vertex_ref);
    const u64 map_offset = cuts_offset+(u64)e->cuts*sizeof(u32)+
        (u64)dc.n_arr*sizeof(*dc.arr)+(u64)dc.n_idx*sizeof(*dc.idx);
    const u64 end = map_offset+(e->remapped ? (u64)e->source_refs*sizeof(u32) : 0);
    if (end > e->mesh_key_size ||
        !ld_early_reserve((void**)&dc.refs, &dc.cap_refs, e->refs, sizeof(*dc.refs)) ||
        !ld_early_reserve((void**)&dc.cuts, &dc.cap_cuts, e->cuts, sizeof(*dc.cuts)) ||
        (e->remapped && !ld_early_reserve((void**)&dc.ref_remap.occurrence_to_unique,
            &dc.ref_remap.occurrence_capacity, e->source_refs, sizeof(u32)))) return 0;
    if (e->refs) memcpy(dc.refs, e->mesh+refs_offset, (size_t)e->refs*sizeof(*dc.refs));
    if (e->cuts) memcpy(dc.cuts, e->mesh+cuts_offset, (size_t)e->cuts*sizeof(*dc.cuts));
    if (e->remapped) memcpy(dc.ref_remap.occurrence_to_unique, e->mesh+map_offset,
        (size_t)e->source_refs*sizeof(u32));
    dc.n_refs = e->refs; dc.n_source_refs = e->source_refs; dc.n_cuts = e->cuts;
    dc.refs_remapped = e->remapped; dc.layout = plan.layout; dc.fetch_plan = plan;
    /* The mesh owns its payload slot, including the invalid slot after an
     * allocation failure. Unrelated geometry cache insertions cannot evict it
     * and force another conversion of a still-live mesh. */
    ld_geometry_entry* changed = e->owns_geometry ? e->geometry : NULL;
    ld_geometry_cache_prepare_record(&dc.fetch_plan, e->mesh, e->mesh_key_size,
        e->mesh_hash, changed);
    return 1;
}
static int ld_early_geometry_prepare(const rsx_vertex_layout_plan* layout)
{
    g_early_geometry.active = g_early_geometry.hit = 0;
    g_early_geometry.mesh_hit = 0;
    g_early_geometry.current_mesh = NULL;
    if (!g_early_geometry.configured)
        rsx_live_draw_set_early_geometry_cache(1);
    if (!g_early_geometry.meshes_configured)
        ld_set_prepared_meshes(1);
    if ((!g_early_geometry.enabled && !g_early_geometry.meshes) ||
        !g_geometry_cache.enabled) return 0;
    rsx_vertex_fetch_plan plan;
    rsx_vertex_fetch_plan_init(&plan, &g.rsx, layout,
        (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    rsx_dsp_index_array indices;
    memset(&indices, 0, sizeof(indices));
    rsx_dsp_get_index_array(&g.rsx, &indices);
    const u32 shape[] = {g.rsx.current_primitive, dc.n_arr, dc.n_idx,
        rsx_dsp_vertex_data_base_index(&g.rsx), plan.base_offset, plan.divider_mask,
        rsx_dsp_restart_index_enabled(&g.rsx, indices.is_u32),
        rsx_dsp_restart_index(&g.rsx), indices.location, indices.offset, indices.is_u32};
    g_early_geometry.size = 0;
#define LD_EARLY_KEY(ptr, count) do { if (!ld_early_append((ptr), (u32)(count))) goto bypass; } while (0)
    LD_EARLY_KEY(shape, sizeof(shape));
    LD_EARLY_KEY(layout, sizeof(*layout));
    if (dc.n_arr > LD_GEOMETRY_KEY_LIMIT/sizeof(*dc.arr) ||
        dc.n_idx > LD_GEOMETRY_KEY_LIMIT/sizeof(*dc.idx)) goto bypass;
    LD_EARLY_KEY(dc.arr, dc.n_arr*sizeof(*dc.arr));
    LD_EARLY_KEY(dc.idx, dc.n_idx*sizeof(*dc.idx));
    for (u32 slot = 0; slot < layout->count; ++slot) {
        const rsx_vertex_fetch_attr* a = &plan.attr[layout->attrs[slot]];
        LD_EARLY_KEY(&a->desc, sizeof(a->desc));
        LD_EARLY_KEY(a->default_value, sizeof(a->default_value));
    }
    g_early_geometry.metadata_size = g_early_geometry.size;
    if (!ld_early_reserve((void**)&g_early_geometry.index_offsets,
            &g_early_geometry.offset_capacity, dc.n_idx, sizeof(u32))) goto bypass;
    const u32 element_size = indices.is_u32 ? 4u : 2u;
    for (u32 batch = 0; batch < dc.n_idx; ++batch) {
        const u64 start = (u64)indices.offset+(u64)dc.idx[batch].first*element_size;
        const u64 bytes = (u64)dc.idx[batch].count*element_size;
        if (start > UINT32_MAX || bytes > LD_GEOMETRY_KEY_LIMIT ||
            start+bytes > 0x100000000ull) goto bypass;
        const u8* source = bytes ? guest_ptr(indices.location, (u32)start, (u32)bytes) : NULL;
        if (bytes && !source) goto bypass;
        g_early_geometry.index_live[batch] = source;
        g_early_geometry.index_bytes[batch] = (u32)bytes;
        if (!g_early_geometry.meshes) {
            g_early_geometry.index_offsets[batch] = g_early_geometry.size;
            LD_EARLY_KEY(source, bytes);
        }
    }
    const u64 hash = ld_early_hash(g_early_geometry.key, g_early_geometry.size);
    g_early_geometry.hash = hash;
    const u32 buckets = g_early_geometry.meshes ? LD_MESH_BUCKETS : LD_EARLY_BUCKETS;
    ld_early_entry* entries = g_early_geometry.meshes ? g_early_geometry.meshes_entries : g_early_geometry.entries;
    const u32 bucket = (u32)(hash ^ (hash >> 32)) & (buckets-1u);
    for (u32 way = 0; way < LD_EARLY_WAYS; ++way) {
        ld_early_entry* entry = &entries[bucket*LD_EARLY_WAYS+way];
        const u32 match_size = g_early_geometry.meshes ? entry->metadata_size : entry->key_size;
        if (entry->hash != hash || match_size != g_early_geometry.size || !entry->key ||
            memcmp(entry->key, g_early_geometry.key, match_size)) continue;
        if (g_early_geometry.meshes) {
            u32 offset = match_size;
            int equal = 1;
            for (u32 batch = 0; batch < dc.n_idx; ++batch) {
                const u32 bytes = g_early_geometry.index_bytes[batch];
                if (offset > entry->key_size || bytes > entry->key_size-offset) { equal=0; break; }
                if (bytes && memcmp(entry->key+offset, g_early_geometry.index_live[batch], bytes)) { equal=0; break; }
                offset += bytes;
            }
            if (!equal || offset != entry->key_size) continue;
        }
        if (!ld_early_restore(entry)) {
            if (!g_early_geometry.meshes || !ld_early_restore_mesh(entry)) break;
            g_early_geometry.mesh_hit = 1;
            g_early_geometry.current_mesh = entry;
            entry->age = ++g_early_geometry.age;
            return 0;
        }
        if (g_early_geometry.meshes) {
            g_early_geometry.current_mesh = entry;
            g_early_geometry.mesh_hit = 1;
        }
        entry->age = ++g_early_geometry.age;
        g_early_geometry.hit = 1;
        return 1;
    }
    /* A miss snapshots indices before decoding. On hits, only compare their
     * existing snapshot; do not copy or hash a potentially large index list. */
    if (g_early_geometry.meshes) for (u32 batch = 0; batch < dc.n_idx; ++batch) {
        g_early_geometry.index_offsets[batch] = g_early_geometry.size;
        LD_EARLY_KEY(g_early_geometry.index_live[batch], g_early_geometry.index_bytes[batch]);
    }
#undef LD_EARLY_KEY
    g_early_geometry.active = 1;
    return 0;
bypass:
    g_early_geometry.active = 0;
    return 0;
}
static void ld_early_geometry_store(void)
{
    ld_geometry_entry* geometry = g_draw_geometry_cache;
    if (g_early_geometry.mesh_hit && g_early_geometry.current_mesh) {
        ld_early_entry* e = g_early_geometry.current_mesh;
        if (geometry && geometry->valid) {
            e->geometry = geometry; e->generation = geometry->generation;
        }
        return;
    }
    if (!g_early_geometry.active || g_early_geometry.hit || !geometry || !geometry->valid) return;
    const u64 hash = g_early_geometry.hash;
    const u32 buckets = g_early_geometry.meshes ? LD_MESH_BUCKETS : LD_EARLY_BUCKETS;
    ld_early_entry* entries = g_early_geometry.meshes ? g_early_geometry.meshes_entries : g_early_geometry.entries;
    const u32 bucket = (u32)(hash ^ (hash >> 32)) & (buckets-1u);
    ld_early_entry* selected = NULL;
    for (u32 way = 0; way < LD_EARLY_WAYS; ++way) {
        ld_early_entry* e = &entries[bucket*LD_EARLY_WAYS+way];
        if (!selected || e->age < selected->age) selected = e;
        if (e->key && e->hash == hash && e->key_size == g_early_geometry.size &&
            !memcmp(e->key, g_early_geometry.key, e->key_size)) { selected = e; break; }
    }
    u64 mesh_size = 0;
    if (g_early_geometry.meshes) {
        mesh_size = ((u64)geometry->key_size+3u)&~3ull;
        mesh_size += geometry->ib_bytes;
        if (mesh_size > LD_GEOMETRY_KEY_LIMIT) return;
    }
    if (g_early_geometry.memory-selected->key_size-selected->mesh_size+
        g_early_geometry.size+mesh_size > LD_EARLY_MEMORY_LIMIT) return;
    u8* mesh = mesh_size ? malloc((size_t)mesh_size) : NULL;
    if (mesh_size && !mesh) return;
    ld_geometry_entry* owned = mesh ? calloc(1, sizeof(*owned)) : NULL;
    if (mesh && !owned) { free(mesh); return; }
    if (mesh) {
        memcpy(mesh, geometry->input, geometry->key_size);
        if (geometry->ib_bytes) write_topology_indices_fast(g.rsx.current_primitive,
            (u32*)(mesh+((geometry->key_size+3u)&~3u)));
    }
    u8* key = realloc(selected->key, g_early_geometry.size);
    if (!key) { free(mesh); free(owned); return; }
    g_early_geometry.memory = g_early_geometry.memory-selected->key_size-selected->mesh_size+
        g_early_geometry.size+mesh_size;
    free(selected->mesh);
    selected->mesh = mesh; selected->mesh_size = (u32)mesh_size;
    selected->mesh_key_size = mesh ? geometry->key_size : 0;
    selected->mesh_hash = geometry->hash;
    selected->topology_count = geometry->topology_count; selected->indexed = geometry->indexed;
    selected->metadata_size = g_early_geometry.meshes ? g_early_geometry.metadata_size : 0;
    memcpy(key, g_early_geometry.key, g_early_geometry.size);
    selected->key = key; selected->key_size = g_early_geometry.size;
    selected->hash = hash; selected->age = ++g_early_geometry.age;
    if (selected->owns_geometry) {
        ld_geometry_entry_evict(selected->geometry);
        free(selected->geometry);
    }
    if (owned) {
        /* Transfer this already prepared immutable version. Its byte budget
         * and in-flight GPU references remain unchanged; only its owner moves. */
        *owned = *geometry;
        memset(geometry, 0, sizeof(*geometry));
        geometry = owned;
        g_draw_geometry_cache = owned;
    }
    selected->geometry = geometry; selected->generation = geometry->generation;
    selected->owns_geometry = owned != NULL;
    selected->refs = dc.n_refs; selected->source_refs = dc.n_source_refs;
    selected->cuts = dc.n_cuts; selected->remapped = dc.refs_remapped;
    selected->plan = dc.fetch_plan;
    g_early_geometry.active = 0;
}
static void ld_early_geometry_destroy(void)
{
    for (u32 i = 0; i < LD_EARLY_BUCKETS*LD_EARLY_WAYS; ++i) {
        free(g_early_geometry.entries[i].key);
        free(g_early_geometry.entries[i].mesh);
    }
    ld_early_mesh_entries_clear();
    free(g_early_geometry.meshes_entries);
    free(g_early_geometry.key); free(g_early_geometry.index_offsets);
    memset(&g_early_geometry, 0, sizeof(g_early_geometry));
}
