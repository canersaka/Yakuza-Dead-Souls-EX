/* Color and depth surfaces.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

static u32 surface_get(u32 location, u32 offset, u32 want_w, u32 want_h)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) {
            if (g.surfaces[i].w == want_w && g.surfaces[i].h == want_h)
                return i;
            slot = i;
            break;
        }
    /* Guest clip dimensions above 8192 are treated as malformed; keep the
     * existing surface rather than replace it with a resource D3D12 would
     * reject. */
    if (want_w > 8192u || want_h > 8192u) {
        static u32 invalid_surface_logs = 0;
        if (invalid_surface_logs++ < 32u)
            fprintf(stderr,
                    "[surface-guard] rejected implausible surface 0x%X "
                    "%ux%u; preserving slot=%s\n",
                    offset, want_w, want_h,
                    slot < MAX_SURFACES ? "existing" : "none");
        return slot < MAX_SURFACES ? slot : LD_INVALID_SURFACE;
    }
    if (slot == MAX_SURFACES) {
        if (g.n_surfaces >= MAX_SURFACES) return LD_INVALID_SURFACE;
        slot = g.n_surfaces;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w; rd.Height = want_h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = rd.Format;
    surface_t* s = &g.surfaces[slot];
    ID3D12Resource* replacement = NULL;
    const HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
        g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        &IID_ID3D12Resource, (void**)&replacement);
    if (FAILED(create_hr)) {
        static u32 surface_fail_logs = 0;
        if (surface_fail_logs++ < 32) {
            const HRESULT removed = g.dev->lpVtbl->GetDeviceRemovedReason(g.dev);
            fprintf(stderr,
                    "[surface-fail] color %u:0x%X %ux%u hr=0x%08lX "
                    "removed=0x%08lX slot=%u count=%u\n",
                    location, offset, want_w, want_h,
                    (unsigned long)create_hr, (unsigned long)removed,
                    slot, g.n_surfaces);
        }
        /* A resize failure must not poison an existing usable slot. */
        return slot < g.n_surfaces && s->tex ? slot : LD_INVALID_SURFACE;
    }
    /* Draws already recorded in the open command list can still reference
     * the old render target.  Release it only after ld_flush fences that list,
     * just like dynamic texture and zeta replacements. */
    if (s->tex)
        retire_texture(s->tex);
    s->tex = replacement;
    s->location = location; s->offset = offset; s->w = want_w; s->h = want_h;
    /* RTVs for surfaces live above the swap-chain backbuffer RTVs */
    g.dev->lpVtbl->CreateRenderTargetView(g.dev, s->tex,
        NULL, rtv_handle(LD_SWAP_BUFFERS + slot));
    srv_write(SRV_SURFACE_BASE + slot, s->tex);
    if (slot == g.n_surfaces) g.n_surfaces++;
    return slot;
}

static u32 current_surface(void)
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    return surface_get(sf.color_location[0], sf.color_offset[0],
                       sf.clip_w, sf.clip_h);
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    g.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g.dsv_heap, &h);
    h.ptr += (size_t)slot * g.dsv_step;
    return h;
}

/* Each RSX zeta address gets its own depth resource; sharing one across
 * shadow, scene and post passes cross-contaminates later depth tests. DSV
 * slot 0 is a shared fallback used only if allocation fails.
 * Returns DSV slot 1+i. */
static u32 zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The DSV must cover the complete live framebuffer/viewport it is bound
     * with.  Some early passes declare a smaller clip than the active canvas;
     * allocating only that clip causes the D3D device to reject later
     * surface/PSO creation.  Sampling a declared-size window from this padded
     * backing remains a separate SRV concern. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    u32 slot = MAX_SURFACES;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        zdepth_t* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) return 1 + i;
            slot = i;
            break;
        }
    }
    if (slot == MAX_SURFACES) {
        if (g.n_zdepths >= MAX_SURFACES) {
            fprintf(stderr, "[zetatrack] live cache full; shared fallback\n");
            return 0;
        }
        slot = g.n_zdepths;
    } else {
        zdepth_t* old = &g.zdepths[slot];
        if (want_w < old->w) want_w = old->w;
        if (want_h < old->h) want_h = old->h;
        if (old->tex) {
            /* Draws already recorded in the open command list may still
             * reference this DSV.  D3D12 command lists do not retain resource
             * lifetimes for the application, so release it only after the
             * next submit/fence completes. */
            retire_texture(old->tex);
            old->tex = NULL;
        }
        if (old->snapshot) {
            retire_texture(old->snapshot);
            old->snapshot = NULL;
        }
    }

    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = want_w;
    rd.Height = want_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    /* Optimized clear values must use the typed DSV format even though the
     * resource itself is typeless so it can also be exposed through an SRV. */
    cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    cv.DepthStencil.Depth = 1.0f;
    zdepth_t* z = &g.zdepths[slot];
    HRESULT create_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->tex);
    if (FAILED(create_hr)) {
        fprintf(stderr,
                "[zetatrack] live create failed %u:0x%X hr=0x%08lX; shared fallback\n",
                location, offset, (unsigned long)create_hr);
        z->tex = NULL;
        return 0;
    }
    z->location = location;
    z->offset = offset;
    z->w = want_w;
    z->h = want_h;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->snapshot_w = rt_w ? rt_w : want_w;
    z->snapshot_h = rt_h ? rt_h : want_h;
    if (z->snapshot_w > want_w) z->snapshot_w = want_w;
    if (z->snapshot_h > want_h) z->snapshot_h = want_h;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = {0};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.dev->lpVtbl->CreateDepthStencilView(
        g.dev, z->tex, &dsvd, dsv_handle(1 + slot));

    /* D3D12 forbids a partial CopyTextureRegion from a depth/stencil
     * resource.  Keep the logical dimensions and resolve the depth plane
     * with a compute shader instead; that is a normal SRV read, not a
     * depth-resource copy operation. */
    D3D12_RESOURCE_DESC snapshot_rd = {0};
    snapshot_rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    snapshot_rd.Width = z->snapshot_w;
    snapshot_rd.Height = z->snapshot_h;
    snapshot_rd.DepthOrArraySize = 1;
    snapshot_rd.MipLevels = 1;
    snapshot_rd.Format = DXGI_FORMAT_R32_FLOAT;
    snapshot_rd.SampleDesc.Count = 1;
    snapshot_rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT snapshot_hr = g.dev->lpVtbl->CreateCommittedResource(
            g.dev, &hp, D3D12_HEAP_FLAG_NONE, &snapshot_rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
            &IID_ID3D12Resource, (void**)&z->snapshot);
    if (FAILED(snapshot_hr)) {
        fprintf(stderr,
                "[zetatrack] live snapshot create failed %u:0x%X "
                "%ux%u hr=0x%08lX; guest fallback\n",
                location, offset, z->snapshot_w, z->snapshot_h,
                (unsigned long)snapshot_hr);
        z->snapshot = NULL;
    } else {
        z->snapshot_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        srv_write_depth_source(SRV_DEPTH_SOURCE_BASE + slot, z->tex);
        srv_write_zdepth(SRV_ZDEPTH_BASE + slot, z->snapshot);
        uav_write_zdepth(UAV_ZDEPTH_BASE + slot, z->snapshot);
    }
    g.list->lpVtbl->ClearDepthStencilView(
        g.list, dsv_handle(1 + slot),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);
    z->cleared = 1;
    if (slot == g.n_zdepths) g.n_zdepths++;
    return 1 + slot;
}

/* Preserve the last completed depth-writing pass before the guest clears or
 * reuses its zeta; the guest never reads depth back to CPU memory, so the GPU
 * result is retained explicitly. */
static int zdepth_snapshot(u32 slot)
{
    if (!slot) return 0;
    zdepth_t* z = &g.zdepths[slot - 1];
    if (!z->snapshot || !g.depth_snapshot_rootsig ||
        !g.depth_snapshot_pso)
        return 0;
    if (!z->had_write) {
        return z->snapshot_valid;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE source_table = {0};
    D3D12_GPU_DESCRIPTOR_HANDLE destination_table = {0};
    if (!depth_snapshot_descriptors(
            slot - 1u, &source_table, &destination_table)) {
        fprintf(stderr,
                "[zetatrack] depth snapshot descriptor ring exhausted "
                "at frame %u\n",
                g_ld_frames);
        return z->snapshot_valid;
    }

    D3D12_RESOURCE_BARRIER bars[2] = {0};
    u32 nbar = 0;
    bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[nbar].Transition.pResource = z->tex;
    bars[nbar].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bars[nbar].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bars[nbar].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    nbar++;
    if (z->snapshot_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        bars[nbar].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[nbar].Transition.pResource = z->snapshot;
        bars[nbar].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[nbar].Transition.StateBefore = z->snapshot_state;
        bars[nbar].Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        nbar++;
    }
    g.list->lpVtbl->ResourceBarrier(g.list, nbar, bars);

    ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.smp_heap};
    g.list->lpVtbl->SetDescriptorHeaps(g.list, 2, heaps);
    g.list->lpVtbl->SetPipelineState(g.list, g.depth_snapshot_pso);
    g.list->lpVtbl->SetComputeRootSignature(
        g.list, g.depth_snapshot_rootsig);
    g.list->lpVtbl->SetComputeRootDescriptorTable(
        g.list, 0, source_table);
    g.list->lpVtbl->SetComputeRootDescriptorTable(
        g.list, 1, destination_table);
    g.list->lpVtbl->Dispatch(
        g.list, (z->snapshot_w + 7u) / 8u,
        (z->snapshot_h + 7u) / 8u, 1);

    D3D12_RESOURCE_BARRIER uav_done[2] = {0};
    uav_done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_done[0].UAV.pResource = z->snapshot;
    u32 uav_count = 1u;
    g.list->lpVtbl->ResourceBarrier(g.list, uav_count, uav_done);

    D3D12_RESOURCE_BARRIER done[2] = {0};
        done[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        done[0].Transition.pResource = z->tex;
        done[0].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        done[0].Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        done[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        done[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        done[1].Transition.pResource = z->snapshot;
        done[1].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        done[1].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        done[1].Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list, 2, done);
    z->snapshot_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    z->snapshot_valid = 1;
    z->had_write = 0;
    return 1;
}
