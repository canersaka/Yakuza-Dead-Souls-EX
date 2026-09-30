/* Frame presentation.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */


void rsx_live_draw_present(u32 buffer_id)
{
    if (!g.ready) return;
    /* A flip names a registered display buffer. The current color target may
     * be an offscreen shadow/postprocess surface at that instant; copying it
     * would present black despite executing the scene's draws. */
    u32 target = LD_INVALID_SURFACE;
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const display_buffer_t* display = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (g.surfaces[i].location == display->location &&
                g.surfaces[i].offset == display->offset) {
                target = i;
                break;
            }
        }
    }
    if (target == LD_INVALID_SURFACE) {
        target = current_surface();
    }
    if (target == LD_INVALID_SURFACE) {
        fprintf(stderr, "[live-draw] frame present skipped: no color surface\n");
        return;
    }
    g_ld_last_present_target = target;
    const u32 current = current_surface();
    surface_t* presented_surface = &g.surfaces[target];
    ID3D12Resource* srcimg = presented_surface->tex;
    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_BARRIER bar[2] = {0};
    bar[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[0].Transition.pResource = srcimg;
    bar[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar[1].Transition.pResource = bb;
    bar[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);

    g.list->lpVtbl->CopyResource(g.list, bb, srcimg);

    bar[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bar[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list, 2, bar);

    ld_flush_mode(LD_FLUSH_PRESENT, 2);
    if (!g.ready) return;
    {
        const HRESULT present_hr = ld_profiled_swap_present();
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames + 1u);
        else {
            fprintf(stderr,
                    "[d3d-fail] Present hr=0x%08lX frame=%u "
                    "target=%u size=%ux%u window=%ux%u\n",
                    (unsigned long)present_hr, g_ld_frames,
                    target, g.surfaces[target].w, g.surfaces[target].h,
                    g.width, g.height);
            ld_dump_dred("Present", present_hr);
            g.ready = 0;
        }
    }

    { static unsigned long long packets_at_last_frame = 0;
      g_ld_last_frame_draws = (u32)(g_ld_stats.packets_seen - packets_at_last_frame);
      packets_at_last_frame = g_ld_stats.packets_seen; }
    g_ld_frames++;

    /* new frame: reset per-frame ring cursors */
    g.vb_used = 0; g.ib_used = 0; g.cb_used = 0; g.ps_cb_used = 0;
    g.srv_ring_used = 0; ld_material_reset_tables(); g.smp_ring_used = 0;
    g.depth_cleared = 0;
    /* Per-zeta resources model persistent RSX memory.  Do not mark them
     * uncleared at a host-frame boundary: the implicit-clear branch would
     * erase depth rendered in an earlier frame and demote had_write before a
     * later pass can sample that zeta.  Only a guest depth clear, handled in
     * sink_clear, may clear or demote a tracked zeta. */
}
