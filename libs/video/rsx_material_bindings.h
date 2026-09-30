/* Included by the retained renderer after texture resolution is defined.
 * Color-target bindings are re-validated on every draw; depth sampling is
 * resolved on every draw, including snapshot/write checks. */
static void ld_material_key(rsx_material_key* key, u32 target)
{
    memcpy(key->words, &g.rsx.regs[0x1840u / 4u], 16u * sizeof(u32));
    memcpy(key->words + 16u, &g.rsx.regs[0x1A00u / 4u], 128u * sizeof(u32));
    key->words[144] = target; /* Changes which color address may be sampled. */
}
static int ld_material_restore(rsx_material_record* record,
    u32 slots[16], u32 samplers[16], u32* mask)
{
    if (!rsx_material_versions_match(record, g_ld_material_surface_epoch,
            g.n_surfaces, g_ld_srv_versions, SRV_CPU_HEAP_SLOTS)) return 0;
    /* Validate all bindings before updating anything. texture_srv_slot
     * owns the once-per-frame content check, including unreadable/failure
     * behavior. A stale check or failed texture always takes that path. */
    for (u32 u = 0; u < 16u; ++u) {
        if (!(record->texture_mask & ~record->dynamic_mask & (1u << u))) continue;
        const u32 slot = record->slots[u];
        if (slot >= SRV_SURFACE_BASE && slot < SRV_SURFACE_BASE + g.n_surfaces)
            continue;
        if (slot < SRV_TEXTURE_BASE || slot >= SRV_TEXTURE_BASE + g.n_textures)
            return 0;
        const texcache_t* texture = &g.textures[slot - SRV_TEXTURE_BASE];
        if (!texture->tex || texture->last_hash_frame != g_ld_frames) return 0;
    }
    memcpy(slots, record->slots, sizeof(record->slots));
    memcpy(samplers, record->samplers, sizeof(record->samplers));
    *mask = record->texture_mask;
    return 1;
}
static void ld_material_touch(u32 slot)
{
    if (slot >= SRV_TEXTURE_BASE && slot < SRV_TEXTURE_BASE + g.n_textures)
        g.textures[slot - SRV_TEXTURE_BASE].last_use_serial = ++g_ld_texture_use_serial;
}
static rsx_material_record* ld_material_save(const rsx_material_key* key,
    u64 hash, const u32 slots[16], const u32 samplers[16], const u64 versions[16],
    u32 mask, u32 dynamic_mask, const u32 surfaces[16], u32 num_surfaces)
{
    if (num_surfaces > 16u) return NULL;
    for (u32 u = 0; u < 16u; ++u) {
        if (dynamic_mask & (1u << u)) continue;
        if (slots[u] >= SRV_CPU_HEAP_SLOTS ||
            versions[u] != g_ld_srv_versions[slots[u]]) return NULL;
        if (mask & (1u << u)) {
            if (slots[u] >= SRV_SURFACE_BASE && slots[u] < SRV_SURFACE_BASE + g.n_surfaces)
                continue;
            if (slots[u] < SRV_TEXTURE_BASE ||
                slots[u] >= SRV_TEXTURE_BASE + g.n_textures ||
                !g.textures[slots[u] - SRV_TEXTURE_BASE].tex) return NULL;
        }
    }
    rsx_material_record* record = rsx_material_store(&g_ld_materials, key, hash);
    record->surface_epoch = g_ld_material_surface_epoch;
    record->surface_count = g.n_surfaces;
    record->texture_mask = mask;
    record->dynamic_mask = dynamic_mask;
    record->num_surfaces = num_surfaces;
    if (num_surfaces) memcpy(record->surfaces, surfaces, num_surfaces * sizeof(u32));
    memcpy(record->slots, slots, sizeof(record->slots));
    memcpy(record->samplers, samplers, sizeof(record->samplers));
    memcpy(record->versions, versions, sizeof(record->versions));
    return record;
}
