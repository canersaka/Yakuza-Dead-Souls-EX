/* Included by rsx_ld_submit.inc. */
#ifndef RSX_LEGACY_FRAME_H
#define RSX_LEGACY_FRAME_H

static int ld_frame_clone_buffer(ID3D12Resource* source,
                                ID3D12Resource** resource, u8** mapped)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES heap = {0};
    D3D12_RANGE read_range = {0, 0};
    if (!source) return 0;
    source->lpVtbl->GetDesc(source, &desc);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &heap,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL, &IID_ID3D12Resource, (void**)resource))) return 0;
    return SUCCEEDED((*resource)->lpVtbl->Map(*resource, 0, &read_range,
                                            (void**)mapped));
}

static void ld_frame_release_retired(ld_legacy_frame* frame)
{
    for (u32 i = 0; i < frame->retired_count; ++i)
        frame->retired[i]->lpVtbl->Release(frame->retired[i]);
    frame->retired_count = 0;
}

/* Only called after the queue has drained (or before any submission at init). */
static void ld_frame_release_storage(ld_legacy_frame* frame)
{
#define LD_FRAME_RELEASE(field) \
    if (frame->field) { frame->field->lpVtbl->Release(frame->field); frame->field = NULL; }
    LD_FRAME_RELEASE(upload)
    LD_FRAME_RELEASE(vb)
    LD_FRAME_RELEASE(ib)
    LD_FRAME_RELEASE(cb)
    LD_FRAME_RELEASE(ps_cb)
    LD_FRAME_RELEASE(srv_heap)
    LD_FRAME_RELEASE(smp_heap)
#undef LD_FRAME_RELEASE
    ld_frame_release_retired(frame);
}

static int ld_frame_storage_init(void)
{
    ld_legacy_frame* first = &g.legacy_frames[0];
    ld_legacy_frame* extra = &g.legacy_frames[1];
#define LD_FRAME_BORROW(field) first->field = g.field
    LD_FRAME_BORROW(upload); LD_FRAME_BORROW(upload_mapped);
    LD_FRAME_BORROW(vb); LD_FRAME_BORROW(vb_mapped);
    LD_FRAME_BORROW(ib); first->ib_mapped = (u8*)g.ib_mapped;
    LD_FRAME_BORROW(cb); LD_FRAME_BORROW(cb_mapped);
    LD_FRAME_BORROW(ps_cb); LD_FRAME_BORROW(ps_cb_mapped);
    LD_FRAME_BORROW(srv_heap); LD_FRAME_BORROW(smp_heap);
#undef LD_FRAME_BORROW
    if (!first->upload_mapped || !first->vb_mapped || !first->ib_mapped ||
        !first->cb_mapped || !first->ps_cb_mapped || !g.alloc_ring[0] ||
        !g.alloc_ring[1]) return 0;
#define LD_FRAME_CLONE(field) \
    if (!ld_frame_clone_buffer(first->field, &extra->field, &extra->field##_mapped)) goto fail
    LD_FRAME_CLONE(upload); LD_FRAME_CLONE(vb); LD_FRAME_CLONE(ib);
    LD_FRAME_CLONE(cb); LD_FRAME_CLONE(ps_cb);
#undef LD_FRAME_CLONE
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    first->srv_heap->lpVtbl->GetDesc(first->srv_heap, &desc);
    if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &desc,
            &IID_ID3D12DescriptorHeap, (void**)&extra->srv_heap))) goto fail;
    first->smp_heap->lpVtbl->GetDesc(first->smp_heap, &desc);
    if (FAILED(g.dev->lpVtbl->CreateDescriptorHeap(g.dev, &desc,
            &IID_ID3D12DescriptorHeap, (void**)&extra->smp_heap))) goto fail;
    return 1;
fail:
    ld_frame_release_storage(extra);
    return 0;
}

/* Publish the current set's last-use fence, then acquire the other set. No
 * upload write, shader-visible descriptor write, release, or allocator reset
 * is permitted until that set's exact fence completes. Queue order protects
 * persistent render targets and depth snapshots without copying them. */
static int ld_frame_rotate(u64 submitted, u32 reason)
{
    ld_legacy_frame* previous = &g.legacy_frames[g.legacy_frame_slot];
    const u32 next_slot = g.legacy_frame_slot ^ 1u;
    ld_legacy_frame* next = &g.legacy_frames[next_slot];
    previous->fence = submitted;
    previous->retired_count = g.n_retired_textures;
    memcpy(previous->retired, g.retired_textures,
           g.n_retired_textures * sizeof(g.retired_textures[0]));
    g.n_retired_textures = 0;
    u64 completed = g.fence->lpVtbl->GetCompletedValue(g.fence);
    if (completed == UINT64_MAX) return 0;
    if (next->fence && completed < next->fence) {
        if (FAILED(g.fence->lpVtbl->SetEventOnCompletion(g.fence,
                next->fence, g.fence_event))) return 0;
        const DWORD status = WaitForSingleObject(g.fence_event, INFINITE);
        completed = g.fence->lpVtbl->GetCompletedValue(g.fence);
        if (status != WAIT_OBJECT_0 || completed == UINT64_MAX ||
            completed < next->fence) return 0;
        g.legacy_reuse_waits++;
    }
    ld_frame_release_retired(next);
    g.legacy_frame_slot = next_slot;
    g.alloc = g.alloc_ring[next_slot];
    if (FAILED(g.alloc->lpVtbl->Reset(g.alloc)) ||
        FAILED(g.list->lpVtbl->Reset(g.list, g.alloc, NULL))) return 0;
#define LD_FRAME_ACQUIRE(field) g.field = next->field
    LD_FRAME_ACQUIRE(upload); LD_FRAME_ACQUIRE(upload_mapped);
    LD_FRAME_ACQUIRE(vb); LD_FRAME_ACQUIRE(vb_mapped);
    LD_FRAME_ACQUIRE(ib); g.ib_mapped = (u32*)next->ib_mapped;
    LD_FRAME_ACQUIRE(cb); LD_FRAME_ACQUIRE(cb_mapped);
    LD_FRAME_ACQUIRE(ps_cb); LD_FRAME_ACQUIRE(ps_cb_mapped);
    LD_FRAME_ACQUIRE(srv_heap); LD_FRAME_ACQUIRE(smp_heap);
#undef LD_FRAME_ACQUIRE
    g.upload_used = g.vb_used = g.ib_used = g.cb_used = g.ps_cb_used = 0;
    g.srv_ring_used = g.smp_ring_used = 0;
    ld_material_reset_tables();
    g.legacy_async_submissions++;
    return 1;
}
#endif
