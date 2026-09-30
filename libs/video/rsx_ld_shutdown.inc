/* Shutdown reports and teardown.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

void rsx_live_draw_shutdown(void)
{
    if (!g.ready) return;
    rsx_live_draw_dump_geometry_cache();
    rsx_prepared_packet_destroy(g_ld_prepared_packets);
    g_ld_prepared_packets = NULL;
    /* let the GPU drain, then release. (Best-effort; process teardown also
     * reclaims.) */
    ld_flush(LD_FLUSH_SHUTDOWN);
    /* Meshes return their owned payloads to the geometry pool. Drain first,
     * then release mesh ownership before destroying that pool and its arenas. */
    ld_early_geometry_destroy();
    ld_pipeline_records_reset();
    ld_geometry_cache_destroy();
    for (u32 i = 0; i < g.n_psos; i++) if (g.psos[i].pso) g.psos[i].pso->lpVtbl->Release(g.psos[i].pso);
    shader_blob_cache_release(&g.vs_blobs);
    shader_blob_cache_release(&g.ps_blobs);
    for (u32 i = 0; i < g.n_textures; i++) if (g.textures[i].tex) g.textures[i].tex->lpVtbl->Release(g.textures[i].tex);
    for (u32 i = 0; i < g.n_vtex; i++) if (g.vtex[i].tex) g.vtex[i].tex->lpVtbl->Release(g.vtex[i].tex);
    for (u32 i = 0; i < g.n_surfaces; i++) if (g.surfaces[i].tex) g.surfaces[i].tex->lpVtbl->Release(g.surfaces[i].tex);
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (g.zdepths[i].tex)
            g.zdepths[i].tex->lpVtbl->Release(g.zdepths[i].tex);
        if (g.zdepths[i].snapshot)
            g.zdepths[i].snapshot->lpVtbl->Release(
                g.zdepths[i].snapshot);
    }
    if (g.white_tex) g.white_tex->lpVtbl->Release(g.white_tex);
    if (g.depth) g.depth->lpVtbl->Release(g.depth);
    ld_frame_release_storage(&g.legacy_frames[0]);
    ld_frame_release_storage(&g.legacy_frames[1]);
    if (g.depth_snapshot_pso)
        g.depth_snapshot_pso->lpVtbl->Release(g.depth_snapshot_pso);
    if (g.depth_snapshot_rootsig)
        g.depth_snapshot_rootsig->lpVtbl->Release(
            g.depth_snapshot_rootsig);
    if (g.rootsig_x) g.rootsig_x->lpVtbl->Release(g.rootsig_x);
    if (g.swap) g.swap->lpVtbl->Release(g.swap);
    if (g_ld_info_queue) {
        g_ld_info_queue->lpVtbl->Release(g_ld_info_queue);
        g_ld_info_queue = NULL;
    }
    if (g.dev) g.dev->lpVtbl->Release(g.dev);
    memset(&g, 0, sizeof(g));
    if (dc.refs) { free(dc.refs); dc.refs = NULL; dc.cap_refs = 0; }
    if (dc.compact_verts) {
        free(dc.compact_verts);
        dc.compact_verts = NULL;
        dc.compact_capacity = 0;
    }
    if (dc.cuts) { free(dc.cuts); dc.cuts = NULL; dc.cap_cuts = 0; }
}
