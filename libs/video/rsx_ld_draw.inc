/* Draw accumulation and execution.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

typedef struct { u32 first, count; } batch_t;

typedef struct {
    batch_t arr[256]; u32 n_arr;
    batch_t idx[256]; u32 n_idx;
    u32     n_packets;
    u32     n_verts;
    rsx_vertex_ref* refs; u32 n_refs, cap_refs;
    u32     n_source_refs;
    int     refs_remapped;
    rsx_vertex_remap ref_remap;
    u8* compact_verts; u64 compact_capacity;
    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int     fetch_ok;
    /* Primitive-restart cut points: a guest index equal to the RSX restart
     * sentinel is a cut marker, not a vertex reference; fetch_batches records
     * the n_verts position at each one and the STRIP/FAN expansion must
     * not bridge across a cut. */
    u32*    cuts; u32 n_cuts, cap_cuts;
} draw_ctx;

static draw_ctx dc;
#include "rsx_geometry_cache.h"
#include "rsx_early_geometry_cache.h"

static void dc_reset(void)
{
    g_early_geometry.active = g_early_geometry.hit = 0;
    g_early_geometry.mesh_hit = 0; g_early_geometry.current_mesh = NULL;
    g_draw_geometry_cache = NULL;
    g_draw_geometry_cache_hit = 0;
    dc.n_arr = dc.n_idx = dc.n_verts = dc.n_refs = 0;
    dc.n_source_refs = 0;
    dc.refs_remapped = 0;
    dc.n_packets = 0;
    dc.n_cuts = 0;
    dc.fetch_ok = 1;
}

static void push_compact_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next_capacity = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* refs = (rsx_vertex_ref*)realloc(
            dc.refs, (size_t)next_capacity * sizeof(*refs));
        if (!refs) {
            dc.fetch_ok = 0;
            return;
        }
        dc.refs = refs;
        dc.cap_refs = next_capacity;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
}

static int reserve_compact_vertices(u64 bytes)
{
    if (bytes <= dc.compact_capacity)
        return 1;
    if (bytes > SIZE_MAX)
        return 0;
    u64 next_capacity = dc.compact_capacity ? dc.compact_capacity : 1u << 20;
    while (next_capacity < bytes) {
        if (next_capacity > SIZE_MAX / 2u)
            return 0;
        next_capacity *= 2u;
    }
    u8* vertices = (u8*)realloc(
        dc.compact_verts, (size_t)next_capacity);
    if (!vertices)
        return 0;
    dc.compact_verts = vertices;
    dc.compact_capacity = next_capacity;
    return 1;
}







static void fetch_batches_hoisted(const rsx_vertex_layout_plan* layout)
{
    if (ld_early_geometry_prepare(layout)) return;
    if (!g_early_geometry.mesh_hit) {
    for (u32 batch = 0; batch < dc.n_arr && dc.fetch_ok; batch++)
        for (u32 i = 0; i < dc.arr[batch].count && dc.fetch_ok; i++)
            push_compact_ref(dc.arr[batch].first + i, 0);

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array index_array;
        rsx_dsp_get_index_array(&g.rsx, &index_array);
        const u32 element_size = index_array.is_u32 ? 4u : 2u;
        const int restart_enabled =
            rsx_dsp_restart_index_enabled(&g.rsx, index_array.is_u32);
        const u32 restart_value = rsx_dsp_restart_index(&g.rsx);

        for (u32 batch = 0; batch < dc.n_idx && dc.fetch_ok; batch++) {
            const u32 first = dc.idx[batch].first;
            const u32 count = dc.idx[batch].count;
            const u64 start64 =
                (u64)index_array.offset + (u64)first * element_size;
            const u64 bytes64 = (u64)count * element_size;
            const u8* contiguous = NULL;
            if (count && start64 <= UINT32_MAX &&
                bytes64 <= UINT32_MAX &&
                start64 + bytes64 <= 0x100000000ull) {
                contiguous = guest_ptr(
                    index_array.location, (u32)start64, (u32)bytes64);
            }
            if (g_early_geometry.active)
                contiguous = g_early_geometry.key+g_early_geometry.index_offsets[batch];
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u32 source_offset =
                    index_array.offset + (first + i) * element_size;
                const u8* source = contiguous
                    ? contiguous + (size_t)i * element_size
                    : guest_ptr(
                        index_array.location, source_offset, element_size);
                if (!source) {
                    dc.fetch_ok = 0;
                    break;
                }
                const u32 index = index_array.is_u32
                    ? (((u32)source[0] << 24) |
                       ((u32)source[1] << 16) |
                       ((u32)source[2] << 8) | source[3])
                    : (u32)((source[0] << 8) | source[1]);
                if (restart_enabled && index == restart_value) {
                    if (!rsx_restart_cut_push(
                            &dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                            dc.n_refs)) {
                        dc.fetch_ok = 0;
                        break;
                    }
                    continue;
                }
                push_compact_ref(index, base_index);
            }
        }
    }

    if (!dc.fetch_ok)
        return;
    dc.n_source_refs = dc.n_refs;
    if (dc.n_refs > 1) {
        u32 unique_refs = dc.n_refs;
        if (rsx_vertex_remap_build(
                &dc.ref_remap, dc.refs, dc.n_refs, &unique_refs)) {
            dc.refs_remapped = unique_refs < dc.n_refs;
            dc.n_refs = unique_refs;
        }
    }
    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(
        &dc.fetch_plan, &g.rsx, layout,
        (rsx_vertex_guest_ptr_fn)g.guest_ptr, g.guest_user);
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    if (ld_geometry_cache_prepare(&dc.fetch_plan)) {
        dc.n_verts = dc.n_refs;
        return;
    }
    }
    if (g_draw_geometry_cache_hit) {
        dc.n_verts = dc.n_refs;
        return;
    }
    const u64 required = (u64)dc.n_refs * layout->stride;
    if (!reserve_compact_vertices(required)) {
        dc.fetch_ok = 0;
        return;
    }
    if (g_ld_batch_fetch) {
        if (!rsx_vertex_fetch_batch(
                &dc.fetch_plan, dc.refs, dc.n_refs, dc.compact_verts)) {
            dc.fetch_ok = 0;
            return;
        }
    } else for (u32 i = 0; i < dc.n_refs; i++) {
        u8* destination = layout->stride
            ? dc.compact_verts + (u64)i * layout->stride : NULL;
        if (!rsx_vertex_fetch_one(
                &dc.fetch_plan, &dc.refs[i], destination)) {
            dc.fetch_ok = 0;
            return;
        }
    }
    dc.n_verts = dc.n_refs;
}



/* primitive ids = raw NV4097 VERTEX_BEGIN_END arg (rsx_dispatch stores it raw;
 * matches the dispatcher). An off-by-one here drops every quad/triangle
 * draw through the switch's default: return -> black. */
#define PRIM_TRIANGLES       5
#define PRIM_TRIANGLE_STRIP  6
#define PRIM_TRIANGLE_FAN    7
#define PRIM_QUADS           8

/* Live-draw activity. DRAW_ARRAYS/DRAW_INDEX_ARRAY writes are packets;
 * the packets between one BEGIN/END are coalesced into a single D3D12 draw
 * (a group). */
typedef struct ld_stats {
    unsigned long long packets_seen;
    unsigned long long groups_executed;
    unsigned long long clears;
} ld_stats;

static ld_stats g_ld_stats;

static void sink_begin(void* u, const rsx_dispatch* r, u32 prim) { (void)u; (void)r; (void)prim; dc_reset(); }
static void sink_draw_arrays(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode) return;
    dc.n_packets++;
    if (dc.n_arr >= 256) return;
    dc.arr[dc.n_arr].first = first; dc.arr[dc.n_arr].count = count; dc.n_arr++;
}
static void sink_draw_index(void* u, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)u; (void)r; g_ld_stats.packets_seen++;
    if (g_ld_movie_mode) return;
    dc.n_packets++;
    if (dc.n_idx >= 256) return;
    dc.idx[dc.n_idx].first = first; dc.idx[dc.n_idx].count = count; dc.n_idx++;
}

static u32 topology_index_count(u32 prim)
{
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES:
        return dc.n_source_refs - dc.n_source_refs % 3u;
    case PRIM_TRIANGLE_STRIP:
    case PRIM_TRIANGLE_FAN: {
        u32 total = 0;
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case PRIM_QUADS:
        return (dc.n_source_refs / 4) * 6;
    default:
        return 0;
    }
}

static u32 topology_vertex_index(u32 occurrence)
{
    return dc.refs_remapped
        ? rsx_vertex_remap_index(&dc.ref_remap, occurrence)
        : occurrence;
}

static void write_topology_indices(u32 prim, u32* indices)
{
    u32 write = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES: {
        const u32 count =
            dc.n_source_refs - dc.n_source_refs % 3u;
        for (u32 occurrence = 0; occurrence < count; occurrence++)
            indices[write++] = topology_vertex_index(occurrence);
        break;
    }
    case PRIM_TRIANGLE_STRIP:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = topology_vertex_index(
                    begin + i + (i & 1u));
                indices[write++] = topology_vertex_index(
                    begin + i + 1u - (i & 1u));
                indices[write++] = topology_vertex_index(begin + i + 2u);
            }
        }
        break;
    case PRIM_TRIANGLE_FAN:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = topology_vertex_index(begin);
                indices[write++] = topology_vertex_index(begin + i);
                indices[write++] = topology_vertex_index(begin + i + 1u);
            }
        }
        break;
    case PRIM_QUADS:
        for (u32 quad = 0; quad < dc.n_source_refs / 4; quad++) {
            const u32 base = quad * 4;
            indices[write++] = topology_vertex_index(base);
            indices[write++] = topology_vertex_index(base + 1u);
            indices[write++] = topology_vertex_index(base + 2u);
            indices[write++] = topology_vertex_index(base + 2u);
            indices[write++] = topology_vertex_index(base + 3u);
            indices[write++] = topology_vertex_index(base);
        }
        break;
    }
}

#define LD_TOPOLOGY_INDEX(o) (map ? map[(o)] : (o))
static void write_topology_indices_fast(u32 prim, u32* indices)
{
    if (dc.refs_remapped && dc.n_source_refs > dc.ref_remap.occurrence_capacity) {
        write_topology_indices(prim, indices);
        return;
    }
    const u32* map = dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL;
    u32 write = 0;
    const u32 segment_count = dc.n_cuts + 1;
    switch (prim) {
    case PRIM_TRIANGLES: {
        const u32 count =
            dc.n_source_refs - dc.n_source_refs % 3u;
        if (!count) break;
        if (map) { memcpy(indices, map, count * sizeof(u32)); break; }
        for (u32 occurrence = 0; occurrence < count; occurrence++)
            indices[write++] = LD_TOPOLOGY_INDEX(occurrence);
        break;
    }
    case PRIM_TRIANGLE_STRIP:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = LD_TOPOLOGY_INDEX(
                    begin + i + (i & 1u));
                indices[write++] = LD_TOPOLOGY_INDEX(
                    begin + i + 1u - (i & 1u));
                indices[write++] = LD_TOPOLOGY_INDEX(begin + i + 2u);
            }
        }
        break;
    case PRIM_TRIANGLE_FAN:
        for (u32 segment = 0; segment < segment_count; segment++) {
            u32 begin, count;
            rsx_restart_segment_bounds(
                dc.cuts, dc.n_cuts, dc.n_source_refs,
                segment, &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = LD_TOPOLOGY_INDEX(begin);
                indices[write++] = LD_TOPOLOGY_INDEX(begin + i);
                indices[write++] = LD_TOPOLOGY_INDEX(begin + i + 1u);
            }
        }
        break;
    case PRIM_QUADS:
        for (u32 quad = 0; quad < dc.n_source_refs / 4; quad++) {
            const u32 base = quad * 4;
            indices[write++] = LD_TOPOLOGY_INDEX(base);
            indices[write++] = LD_TOPOLOGY_INDEX(base + 1u);
            indices[write++] = LD_TOPOLOGY_INDEX(base + 2u);
            indices[write++] = LD_TOPOLOGY_INDEX(base + 2u);
            indices[write++] = LD_TOPOLOGY_INDEX(base + 3u);
            indices[write++] = LD_TOPOLOGY_INDEX(base);
        }
        break;
    }
}


#undef LD_TOPOLOGY_INDEX

static void sink_end_impl(void* user, const rsx_dispatch* r)
{
#define LD_DRAW_ABORT() return
    (void)user; (void)r;
    if (g_ld_movie_mode)
        LD_DRAW_ABORT();
    if (!dc.n_packets) {
        LD_DRAW_ABORT();
    }
    const u32 prim = g.rsx.current_primitive;
    rsx_vertex_layout_plan used_layout;
    ld_current_vertex_layout(&used_layout);
    int indexed = 0;
    u32 n_tri = 0;
    fetch_batches_hoisted(&used_layout);
    if (!dc.n_source_refs)
        dc.n_source_refs = dc.n_verts;
    if (!dc.n_verts || !dc.fetch_ok) {
        LD_DRAW_ABORT();
    }

    /* Preserve validated occurrence order while allowing repeated compact
     * vertex references to share one fetched and uploaded payload. */
    if (g_early_geometry.mesh_hit && g_early_geometry.current_mesh) {
        indexed=g_early_geometry.current_mesh->indexed;
        n_tri=g_early_geometry.current_mesh->topology_count;
    } else {
    if (!rsx_vertex_topology_plan(prim, dc.refs_remapped, &indexed)) {
        LD_DRAW_ABORT();
    }
    n_tri = indexed
        ? topology_index_count(prim)
        : dc.n_source_refs - dc.n_source_refs % 3;
    }

    if (!n_tri) {
        LD_DRAW_ABORT();
    }


    const u32 vertex_stride = used_layout.stride;
    const u32 uploaded_vertices = indexed ? dc.n_verts : n_tri;
    const void* vertex_data = dc.compact_verts;
    const u64 draw_vb_bytes = (u64)uploaded_vertices * vertex_stride;
    const u64 draw_ib_bytes = indexed ? (u64)n_tri * sizeof(u32) : 0;
    const u64 vb_capacity = VERT_BUFFER_SIZE;
    if (draw_vb_bytes > vb_capacity || draw_ib_bytes > INDEX_BUFFER_SIZE) {
        LD_DRAW_ABORT();
    }
    if ((u64)g.vb_used + draw_vb_bytes > vb_capacity ||
        (u64)g.ib_used + draw_ib_bytes > INDEX_BUFFER_SIZE) {
        /* Recycle the transient upload rings here instead of dropping the
         * remaining draws. Nothing for this draw has been copied yet, so it
         * is safe to submit, wait and reuse every ring. */
        ld_flush(LD_FLUSH_VERTEX_RING);
        g.vb_used = 0;
        g.ib_used = 0;
        g.cb_used = 0;
        g.ps_cb_used = 0;
        g.srv_ring_used = 0; ld_material_reset_tables();
        g.smp_ring_used = 0;
    }
    if (draw_vb_bytes && !g_draw_geometry_cache_hit)
        memcpy(
            g.vb_mapped + g.vb_used, vertex_data,
            (size_t)draw_vb_bytes);
    void* geometry_index_source = (u8*)g.ib_mapped + g.ib_used;
    if (indexed) {
        u32* index_destination = ld_geometry_index_destination(geometry_index_source, draw_ib_bytes);
        geometry_index_source = index_destination;
        if (g_draw_geometry_cache_hit) { /* immutable cached index buffer */ }
        else if (g_early_geometry.mesh_hit && g_early_geometry.current_mesh) {
            const ld_early_entry* mesh = g_early_geometry.current_mesh;
            memcpy(index_destination, mesh->mesh+((mesh->mesh_key_size+3u)&~3u), (size_t)draw_ib_bytes);
        }
        else
        if (g_ld_fast_indices)
            write_topology_indices_fast(prim, index_destination);
        else
            write_topology_indices(prim, index_destination);
    }
    ld_geometry_cache_store(vertex_data, draw_vb_bytes,
        geometry_index_source, draw_ib_bytes,n_tri,indexed);
    ld_early_geometry_store();
    if (geometry_index_source != (u8*)g.ib_mapped + g.ib_used &&
        (!g_draw_geometry_cache || !g_draw_geometry_cache->valid))
        memcpy((u8*)g.ib_mapped + g.ib_used, geometry_index_source, (size_t)draw_ib_bytes);

    ID3D12PipelineState* pso = get_pso(&used_layout, 1);
    if (!pso) {
        LD_DRAW_ABORT();
    }
    u32 vertex_cb_offset = 0;
    int vertex_cb_plan = rsx_vertex_constant_ring_plan(
        g.cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED, &vertex_cb_offset);
    if (vertex_cb_plan == 0) {
        ld_flush(LD_FLUSH_VERTEX_CONSTANT_RING);
        if (!g.ready)
            LD_DRAW_ABORT();
        g.cb_used = 0;
        vertex_cb_plan = rsx_vertex_constant_ring_plan(
            g.cb_used, CB_RING_BYTES, CB_BLOCK_ALIGNED,
            &vertex_cb_offset);
    }
    if (vertex_cb_plan != 1) {
        LD_DRAW_ABORT();
    }
    g.cb_used = vertex_cb_offset;
    u32 ps_cb_offset = 0;
    if (!ld_upload_pixel_constants(&ps_cb_offset)) {
        LD_DRAW_ABORT();
    }

    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) {
        LD_DRAW_ABORT();
    }
    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 current_zslot = g.depth
        ? zdepth_get(sf.zeta_location, sf.zeta_offset, sf.clip_w, sf.clip_h)
        : 0;
    u32 slots[SRV_TABLE_SIZE], smp_slots[SMP_TABLE_SIZE];
    u32 surf_used[SRV_TABLE_SIZE], n_surf_used = 0;
    u32 zdepth_used[SRV_TABLE_SIZE], n_zdepth_used = 0;
    u32 texture_mask = 0;
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) slots[u] = SRV_WHITE;
    for (u32 u = 0; u < SMP_TABLE_SIZE; u++) smp_slots[u] = SMP_DEFAULT;
    rsx_material_key material_key;
    u64 material_hash = 0, material_versions[16];
    rsx_material_record* material_record = NULL;
    int material_hit = 0;
    u32 material_dynamic_mask = 0;
    {
        ld_material_key(&material_key, target);
        material_hash = rsx_material_hash(&material_key);
        material_record = rsx_material_find(&g_ld_materials, &material_key, material_hash);
        material_hit = ld_material_restore(material_record, slots, smp_slots, &texture_mask);
        if (material_hit) {
            material_dynamic_mask = material_record->dynamic_mask;
            n_surf_used = material_record->num_surfaces;
            memcpy(surf_used, material_record->surfaces, n_surf_used * sizeof(u32));
        }
    }
    for (u32 u = 0; u < 16u; ++u) material_versions[u] = g_ld_srv_versions[SRV_WHITE];
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++) {
        if (material_hit && !(material_dynamic_mask & (1u << u))) {
            /* A preceding dynamic unit may have evicted a later static unit.
             * Preserve original per-unit lookup/LRU ordering in that case. */
            if (g_ld_srv_versions[slots[u]] == material_record->versions[u]) {
                if (texture_mask & (1u << u)) ld_material_touch(slots[u]);
                material_versions[u] = g_ld_srv_versions[slots[u]];
                continue;
            }
            material_hit = 0;
            material_record = NULL;
        }
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (!t.enabled) continue;
        texture_mask |= 1u << u;
        smp_slots[u] = sampler_slot(&t, sampler_key(&t));
        int sampled = -1;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (g.surfaces[i].location != t.location || g.surfaces[i].offset != t.offset) continue;
            if (i != target) { sampled = (int)i; break; }
        }
        if (sampled >= 0) {
            slots[u] = SRV_SURFACE_BASE + sampled;
            int seen = 0;
            for (u32 k = 0; k < n_surf_used; k++) if (surf_used[k] == (u32)sampled) seen = 1;
            if (!seen && n_surf_used < SRV_TABLE_SIZE) surf_used[n_surf_used++] = (u32)sampled;
        } else {
            int sampled_depth = -1;
            const u32 texture_base_fmt =
                t.format & TEX_FMT_BASE_MASK & ~(u32)TEX_FMT_UNNORM;
            if (texture_base_fmt == TEX_FMT_DEPTH24_D8) material_dynamic_mask |= 1u << u;
            /* A depth texture binds a tracked zeta only when address and
             * format match and a depth-writing draw ran. Clear-only zetas
             * read guest memory instead. */
            if (texture_base_fmt == TEX_FMT_DEPTH24_D8)
                for (u32 i = 0; i < g.n_zdepths; i++)
                    if (g.zdepths[i].location == t.location &&
                        g.zdepths[i].offset == t.offset &&
                        current_zslot != 1 + i) {
                        if (zdepth_snapshot(1 + i))
                            sampled_depth = (int)i;

                        break;
                    }
            if (sampled_depth >= 0) {
                slots[u] = SRV_ZDEPTH_BASE + sampled_depth;
                int seen = 0;
                for (u32 k = 0; k < n_zdepth_used; k++)
                    if (zdepth_used[k] == (u32)sampled_depth) seen = 1;
                if (!seen && n_zdepth_used < SRV_TABLE_SIZE)
                    zdepth_used[n_zdepth_used++] = (u32)sampled_depth;
            } else {
                slots[u] = texture_srv_slot(&t);
            }
        }
        material_versions[u] = g_ld_srv_versions[slots[u]];
    }
    {
        if (!material_hit)
            material_record = ld_material_save(&material_key, material_hash,
                slots, smp_slots, material_versions, texture_mask, material_dynamic_mask,
                surf_used, n_surf_used);
    }

    const u32 vtex_mask = vertex_texture_mask();
    u32 vtex_slots[SRV_TABLE_SIZE];
    for (u32 u = 0; u < SRV_TABLE_SIZE; u++)
        vtex_slots[u] = SRV_WHITE;
    /* Resolve and upload every vertex texture before recording this draw's
     * graphics state.  Cache refresh can retire a texture and flush the open
     * command list; doing that after SetPipelineState/root bindings silently
     * loses those bindings on the replacement list. */
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        vtex_slots[u] = vertex_texture_srv_slot(&vt);
    }
    const u32 required_srv_tables = 1u + (vtex_mask != 0);
    if (g.srv_ring_used + required_srv_tables > SRV_RING_TABLES ||
        sampler_table_needs_flush(smp_slots)) {
        /* Every descriptor referenced by the old command list is consumed
         * before recycling its shader-visible heap entries. Texture upload
         * commands recorded while resolving slots are also completed here;
         * no render-state command for this draw has been recorded yet. */
        ld_flush(LD_FLUSH_DESCRIPTOR_RING);
        if (!g.ready) LD_DRAW_ABORT();
        g.srv_ring_used = 0; ld_material_reset_tables();
        g.smp_ring_used = 0;
    }

    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv; int have_dsv = 0;
    if (g.depth) {
        dsv = dsv_handle(current_zslot);
        have_dsv = 1;
        if (current_zslot) {
            zdepth_t* z = &g.zdepths[current_zslot - 1];
            if (!z->cleared) {
                g.list->lpVtbl->ClearDepthStencilView(
                    g.list, dsv,
                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f, 0, 0, NULL);
                z->cleared = 1;
                z->had_write = 0;
            }
        } else if (!g.depth_cleared) {
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
            g.depth_cleared = 1;
        }
    }
    g.list->lpVtbl->OMSetRenderTargets(g.list, 1, &rtv, FALSE, have_dsv ? &dsv : NULL);
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE table = {0}, stbl = {0};
    const int material_bindings_valid =
        rsx_material_versions_match(material_record,
            g_ld_material_surface_epoch, g.n_surfaces,
            g_ld_srv_versions, SRV_CPU_HEAP_SLOTS) &&
        rsx_material_update_dynamic(material_record, slots, g_ld_srv_versions, SRV_CPU_HEAP_SLOTS);
    if (material_bindings_valid) {
        table.ptr = rsx_material_table_handle(material_record, g_ld_material_table_epoch);
        if (table.ptr) stbl.ptr = material_record->sampler_handle;
    }
    if (!table.ptr || !stbl.ptr) {
        table = srv_table(slots);
        stbl = sampler_table(smp_slots);
        if (material_bindings_valid && table.ptr && stbl.ptr) {
            material_record->table_epoch = g_ld_material_table_epoch;
            material_record->table_handle = table.ptr;
            material_record->sampler_handle = stbl.ptr;
        }
    }
    D3D12_GPU_DESCRIPTOR_HANDLE vtex_table = {0};
    if (vtex_mask)
        vtex_table = srv_table(vtex_slots);
    u64 descriptor_signature =
        rsx_fnv1a(slots, sizeof(slots), RSX_FNV64_OFFSET);
    descriptor_signature =
        rsx_fnv1a(smp_slots, sizeof(smp_slots), descriptor_signature);
    if (vtex_mask)
        descriptor_signature =
            rsx_fnv1a(vtex_slots, sizeof(vtex_slots), descriptor_signature);

    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    float xf[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (W * 0.5f);
        xf[1] = -(vp.scale[1] / (H * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - W * 0.5f) / (W * 0.5f);
        xf[5] = -((vp.translate[1] - H * 0.5f) / (H * 0.5f));
        xf[6] = vp.translate[2];
    }
    u8* cbdst = g.cb_mapped + g.cb_used;
    memcpy(cbdst, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16);
    memcpy(cbdst + 512 * 16, xf, sizeof(xf));
    /* Vertex-program branch instructions read this dynamic word (register
     * 0x1FF8) after the two viewport vectors. Keep the padding initialized
     * too, including after constant-ring reuse. */
    const u32 branch_words[4] = {rsx_dsp_reg(&g.rsx, 0x1FF8u), 0u, 0u, 0u};
    memcpy(cbdst + (512 + 2) * 16, branch_words, sizeof(branch_words));

    g.list->lpVtbl->SetPipelineState(g.list, pso);
    g.list->lpVtbl->SetGraphicsRootSignature(g.list, g.rootsig_x);
    g.list->lpVtbl->SetGraphicsRootConstantBufferView(
        g.list, 0, g.cb->lpVtbl->GetGPUVirtualAddress(g.cb) + g.cb_used);
    g.list->lpVtbl->SetGraphicsRootConstantBufferView(
            g.list, 4,
            g.ps_cb->lpVtbl->GetGPUVirtualAddress(g.ps_cb) +
                ps_cb_offset);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 1, table);
    g.list->lpVtbl->SetGraphicsRootDescriptorTable(g.list, 2, stbl);
    if (vtex_mask)
        g.list->lpVtbl->SetGraphicsRootDescriptorTable(
            g.list, 3, vtex_table);
    g.cb_used += CB_BLOCK_ALIGNED;

    D3D12_VIEWPORT dvp = {0, 0, W, H, 0.0f, 1.0f};
    D3D12_RECT sc = {
        0, 0,
        (LONG)(g.surfaces[target].w ? g.surfaces[target].w : g.width),
        (LONG)(g.surfaces[target].h ? g.surfaces[target].h : g.height)
    };
    /* Guest scissor, intersected with the surface rect. The nv40 reset value
     * is a full-window 4096x4096 (never-written regs read 0 here, which the
     * w==0 test treats as "no scissor"), so ordinary streams keep the
     * full-surface rect and only genuine game scissors narrow it. */
    {
        const u32 sh = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 sv = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const LONG sx = (LONG)(sh & 0xFFFFu), sw = (LONG)(sh >> 16);
        const LONG sy = (LONG)(sv & 0xFFFFu), svh = (LONG)(sv >> 16);
        if (sw > 0 && svh > 0) {
            if (sx > sc.left)            sc.left   = sx;
            if (sy > sc.top)             sc.top    = sy;
            if (sx + sw  < sc.right)     sc.right  = sx + sw;
            if (sy + svh < sc.bottom)    sc.bottom = sy + svh;
            if (sc.right  < sc.left)     sc.right  = sc.left;
            if (sc.bottom < sc.top)      sc.bottom = sc.top;
        }
    }
    g.list->lpVtbl->RSSetViewports(g.list, 1, &dvp);
    g.list->lpVtbl->RSSetScissorRects(g.list, 1, &sc);
    /* Dynamic stencil reference (kept out of the PSO key on purpose). */
    g.list->lpVtbl->OMSetStencilRef(
        g.list, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);

    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = g.vb->lpVtbl->GetGPUVirtualAddress(g.vb) + g.vb_used;
    vbv.StrideInBytes = vertex_stride;
    vbv.SizeInBytes = (u32)draw_vb_bytes;
    if (g_draw_geometry_cache && g_draw_geometry_cache->valid)
        vbv.BufferLocation = g_draw_geometry_cache->resource->lpVtbl->GetGPUVirtualAddress(g_draw_geometry_cache->resource)
            + g_draw_geometry_cache->buffer->resource_offset;
    if (vertex_stride)
        g.list->lpVtbl->IASetVertexBuffers(g.list, 0, 1, &vbv);
    g.list->lpVtbl->IASetPrimitiveTopology(g.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        const u64 serial = ++g_ld_recent_draw_total;
        ld_recent_draw* draw =
            &g_ld_recent_draws[(serial - 1u) & (LD_RECENT_DRAW_CAP - 1u)];
        memset(draw, 0, sizeof(*draw));
        draw->serial = serial;
        draw->frame = g_ld_frames;
        draw->pso_key = g_ld_current_pso.key;
        draw->descriptor_signature = descriptor_signature;
        draw->vp_start = g_ld_current_pso.vp_start;
        draw->vp_instrs = g_ld_current_pso.vp_instrs;
        draw->fp_location = g_ld_current_pso.fp_location;
        draw->fp_offset = g_ld_current_pso.fp_offset;
        draw->fp_size = g_ld_current_pso.fp_size;
        draw->fp_control = g_ld_current_pso.fp_control;
        draw->cube_mask = g_ld_current_pso.cube_mask;
        draw->vtex_mask = g_ld_current_pso.vtex_mask;
        draw->txl_mask = g_ld_current_pso.txl_mask;
        draw->texture_mask = texture_mask;
        draw->primitive = prim;
        draw->vertices = n_tri;
        draw->target = target;
        draw->zslot = current_zslot;
        draw->clip_x = sf.clip_x;
        draw->clip_y = sf.clip_y;
        draw->clip_w = sf.clip_w;
        draw->clip_h = sf.clip_h;
        draw->srv_ring_used = g.srv_ring_used;
        draw->sampler_ring_used = g.smp_ring_used;
        draw->cb_used = g.cb_used;
        draw->vb_used = g.vb_used;
    }
    if (indexed) {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation =
            g.ib->lpVtbl->GetGPUVirtualAddress(g.ib) + g.ib_used;
        ibv.SizeInBytes = (u32)draw_ib_bytes;
        ibv.Format = DXGI_FORMAT_R32_UINT;
        if (g_draw_geometry_cache && g_draw_geometry_cache->valid)
            ibv.BufferLocation = g_draw_geometry_cache->resource->lpVtbl->GetGPUVirtualAddress(g_draw_geometry_cache->resource)
                + g_draw_geometry_cache->ib_offset + g_draw_geometry_cache->buffer->resource_offset;
        g.list->lpVtbl->IASetIndexBuffer(g.list, &ibv);
        g.list->lpVtbl->DrawIndexedInstanced(
            g.list, n_tri, 1, 0, 0, 0);
    } else {
        g.list->lpVtbl->DrawInstanced(g.list, n_tri, 1, 0, 0);
    }
    if (current_zslot) {
        render_state_t depth_state;
        decode_render_state(&depth_state);
        zdepth_t* z = &g.zdepths[current_zslot - 1];
        z->draws++;
        if (depth_state.depth_test) z->depth_test_draws++;
        if (depth_state.depth_write) z->depth_write_draws++;
        if (depth_state.depth_test && depth_state.depth_write)
            z->depth_both_draws++;
        /* A write-enable bit alone does not prove that this draw produced a
         * usable depth map.  With depth testing disabled RSX does not execute
         * the depth pass represented by this tracked zeta. */
        if (depth_state.depth_test && depth_state.depth_write) {
            z->had_write = 1;
        }
    }
    g_ld_stats.groups_executed++;

    for (u32 k = 0; k < n_surf_used; k++) {
        bar.Transition.pResource = g.surfaces[surf_used[k]].tex;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list, 1, &bar);
    }
    g.vb_used += (u32)draw_vb_bytes;
    g.ib_used += (u32)draw_ib_bytes;
#undef LD_DRAW_ABORT
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    sink_end_impl(user, r);
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (g_ld_movie_mode) return;
    g_ld_stats.clears++;
    const u32 target = current_surface();
    if (target == LD_INVALID_SURFACE) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(LD_SWAP_BUFFERS + target);
    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G | RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                               (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f };
        g.list->lpVtbl->ClearRenderTargetView(g.list, rtv, col, 0, NULL);
    }
    if ((mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) && g.depth) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(zslot);
        D3D12_CLEAR_FLAGS clear_flags = (D3D12_CLEAR_FLAGS)0;
        if (mask & RSX_CLEAR_DEPTH)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_DEPTH);
        if (mask & RSX_CLEAR_STENCIL)
            clear_flags = (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_STENCIL);
        /* ZSTENCIL_CLEAR_VALUE (Z24S8 layout: depth24 << 8 | stencil8; nv40
         * reset 0xFFFFFF00 = depth 1.0, stencil 0, so never-written streams
         * clear to 1.0/0). */
        {
            const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
            const float cd = (float)(zs >> 8) / 16777215.0f;
            g.list->lpVtbl->ClearDepthStencilView(g.list, dsv,
                clear_flags, zs ? cd : 1.0f, (UINT8)(zs & 0xFFu), 0, NULL);
        }
        if (zslot) {
            g.zdepths[zslot - 1].cleared = 1;
            if (mask & RSX_CLEAR_DEPTH) {
                g.zdepths[zslot - 1].had_write = 0;
                /* Each guest zeta owns a distinct live resource.  A clear
                 * invalidates its older published image; do not resolve it
                 * speculatively.  The first later texture consumer resolves
                 * the newly written pass exactly once. */
                g.zdepths[zslot - 1].snapshot_valid = 0;
            }
        } else {
            if (mask & RSX_CLEAR_DEPTH)
                g.depth_cleared = 1;
        }
    }
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    if (g_ld_movie_mode) return;
    rsx_live_draw_present(arg & 7u);
}
