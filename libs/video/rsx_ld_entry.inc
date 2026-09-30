/* Method entry points, statistics and movie frames.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

void rsx_live_draw_seed_registers(const u32* regs, u32 count)
{
    if (g.ready) rsx_dispatch_seed_registers(&g.rsx, regs, count);
}
void rsx_live_draw_seed_transform_program(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_program(&g.rsx, words, count);
}
void rsx_live_draw_seed_transform_constants(const u32* words, u32 count)
{
    if (g.ready) rsx_dispatch_seed_transform_constants(&g.rsx, words, count);
}

void rsx_live_draw_set_display_buffer(
    u32 buffer_id, u32 location, u32 offset, u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    display_buffer_t* display = &g.display_buffers[buffer_id];
    display->location = location;
    display->offset = offset;
    display->pitch = pitch;
    display->width = width;
    display->height = height;
    display->valid = width && height;
}

int rsx_live_draw_prepared_packet_supported(u32 header)
{
    return g.ready && g_ld_prepared_packets && !g_ld_movie_mode && !g_ld_host_waiting &&
        !g_ld_debug_layer_enabled &&
        rsx_prepared_packet_supported(g_ld_prepared_packets,header);
}
int rsx_live_draw_graphics_packet(u32 header,const u32* arguments,rsx_state* protocol)
{
    if(!g.ready || !g_ld_prepared_packets || g_ld_movie_mode || g_ld_host_waiting ||
        g_ld_debug_layer_enabled)return 0;
    return rsx_prepared_packet_execute_graphics(g_ld_prepared_packets,&g.rsx,protocol,header,arguments,
        NULL);
}

void rsx_live_draw_method(u32 method, u32 arg)
{
    /* With the D3D12 debug layer on, the host movie thread and the FIFO
     * consumer share the command list; serialize them. */
    if (g_ld_debug_layer_enabled) {
        for (;;) {
            if (g_ld_movie_mode || g_ld_host_waiting) {
                while (g_ld_host_waiting)
                    SwitchToThread();
                AcquireSRWLockExclusive(&g_ld_access_lock);
                if (g.ready && !g_ld_movie_mode) {
                    rsx_dispatch_method(&g.rsx, method, arg);
                }
                ReleaseSRWLockExclusive(&g_ld_access_lock);
                return;
            }
            InterlockedIncrement(&g_ld_guest_active);
            MemoryBarrier();
            if (!g_ld_movie_mode && !g_ld_host_waiting) {
                if (g.ready) {
                    rsx_dispatch_method(&g.rsx, method, arg);
                }
                InterlockedDecrement(&g_ld_guest_active);
                return;
            }
            InterlockedDecrement(&g_ld_guest_active);
        }
    }

    if (!g.ready || g_ld_movie_mode) return;
    rsx_dispatch_method(&g.rsx, method, arg);
}

void rsx_live_draw_set_movie_mode(int on)
{
    const int serialized = g_ld_debug_layer_enabled;
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        if (on) InterlockedExchange((volatile LONG*)&g_ld_movie_mode, 1);
        while (InterlockedCompareExchange(&g_ld_guest_active, 0, 0) != 0)
            SwitchToThread();
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    g_ld_movie_mode = on ? 1 : 0;
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}

u32 rsx_live_draw_get_frames(void) { return g_ld_frames; }
u64 rsx_live_draw_get_completed_draws(void)
{
    return (u64)g_ld_stats.groups_executed;
}

/* Draws in the last completed frame (title-bar telemetry). */
u32 rsx_live_draw_get_last_draws(void) { return g_ld_last_frame_draws; }

double rsx_live_draw_get_present_fps(void)
{
    const u64 total = g_ld_present_total;
    if (total < 2u || g_ld_qpc_frequency <= 0)
        return 0.0;

    const u64 available =
        total < LD_PRESENT_RING_CAP ? total : LD_PRESENT_RING_CAP;
    const ld_present_sample* newest =
        &g_ld_present_ring[(total - 1u) & (LD_PRESENT_RING_CAP - 1u)];
    if (newest->present_id != total)
        return 0.0;

    const ld_present_sample* oldest = newest;
    u64 intervals = 0;
    for (u64 back = 1u; back < available; ++back) {
        const u64 present_id = total - back;
        const ld_present_sample* candidate =
            &g_ld_present_ring[(present_id - 1u) &
                               (LD_PRESENT_RING_CAP - 1u)];
        if (candidate->present_id != present_id)
            break;
        oldest = candidate;
        intervals = back;
        const double elapsed =
            (double)(newest->qpc - oldest->qpc) /
            (double)g_ld_qpc_frequency;
        /*
         * Keep at least a 15-second view and about 30 intervals when possible;
         * cap at 30 seconds so sub-1-FPS operation still updates usefully.
         */
        if (elapsed >= 30.0 || (elapsed >= 15.0 && intervals >= 30u))
            break;
    }

    const LONGLONG ticks = newest->qpc - oldest->qpc;
    return intervals != 0u && ticks > 0
        ? (double)intervals * (double)g_ld_qpc_frequency / (double)ticks
        : 0.0;
}

/* Present a host-decoded RGBA8 frame to the window: copy it straight into the
 * swap-chain backbuffer (both R8G8B8A8_UNORM at the swap size) and Present.
 * The frame is clamped to the backbuffer size. Call from a single thread with
 * movie mode on (so guest draws don't touch g.list). */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h)
{
    const int serialized = g_ld_debug_layer_enabled;
    if (serialized) {
        InterlockedExchange(&g_ld_host_waiting, 1);
        AcquireSRWLockExclusive(&g_ld_access_lock);
    }
    if (!g.ready || !rgba) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }
    if (w > g.width)  w = g.width;
    if (h > g.height) h = g.height;
    const u32 pitch = (w * 4 + 255) & ~255u;          /* D3D12 copy pitch align */
    if ((UINT64)pitch * h > UPLOAD_SIZE) {
        if (serialized) {
            ReleaseSRWLockExclusive(&g_ld_access_lock);
            InterlockedExchange(&g_ld_host_waiting, 0);
        }
        return;
    }

    for (u32 y = 0; y < h; y++)
        memcpy(g.upload_mapped + (size_t)y * pitch,
               rgba + (size_t)y * w * 4, (size_t)w * 4);

    const u32 bbi = g.swap->lpVtbl->GetCurrentBackBufferIndex(g.swap);
    ID3D12Resource* bb = g.backbuf[bbi];

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = g.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    g.list->lpVtbl->CopyTextureRegion(g.list, &dst, 0, 0, 0, &src, NULL);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.list->lpVtbl->ResourceBarrier(g.list, 1, &b);

    ld_flush(LD_FLUSH_MOVIE_PRESENT);
    {
        const HRESULT present_hr = ld_profiled_swap_present();
        if (SUCCEEDED(present_hr))
            ld_present_measure_record(g_ld_frames);
        else {
            fprintf(stderr,
                    "[d3d-fail] movie Present hr=0x%08lX frame=%u\n",
                    (unsigned long)present_hr, g_ld_frames);
            ld_dump_dred("MoviePresent", present_hr);
            g.ready = 0;
        }
    }
    if (serialized) {
        ReleaseSRWLockExclusive(&g_ld_access_lock);
        InterlockedExchange(&g_ld_host_waiting, 0);
    }
}
