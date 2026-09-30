/* Two bounded, independently owned sets of GPU-visible transient storage. */
#ifndef RSX_LEGACY_FRAME_TYPES_H
#define RSX_LEGACY_FRAME_TYPES_H
typedef struct ld_legacy_frame {
    ID3D12Resource *upload, *vb, *ib, *cb, *ps_cb;
    u8 *upload_mapped, *vb_mapped, *ib_mapped, *cb_mapped, *ps_cb_mapped;
    ID3D12DescriptorHeap *srv_heap, *smp_heap;
    ID3D12Resource* retired[MAX_RETIRED_TEXTURES];
    u32 retired_count;
    u64 fence;
} ld_legacy_frame;
#endif
