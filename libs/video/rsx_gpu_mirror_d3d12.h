/*
 * ps3recomp - D3D12 backend for the persistent GPU guest-memory mirror
 *
 * Owns one DEFAULT-heap raw buffer per guest space (the ByteAddressBuffer
 * SRVs the vertex-pulling shaders read at t20/t21) plus a persistently
 * mapped UPLOAD staging area split into three fence-tracked slices, so a
 * sync can never overwrite staging bytes a queued frame still copies from.
 *
 * Session contract (all on the render thread, once per frame):
 *   retire(fence->GetCompletedValue());
 *   begin(command_list);              // transitions buffers to COPY_DEST
 *   rsx_gpu_mirror_sync(mirror, ..);  // upload() memcpys into the slice
 *                                     // and records CopyBufferRegion
 *   end(fence_value_to_be_signaled);  // transitions back to SRV state
 *   ... record draws, execute, Signal(fence_value) ...
 *
 * If the current slice's previous use has not been retired yet, or a sync
 * outgrows the slice, upload() rejects and the mirror keeps those pages
 * dirty for the next frame — bounded staging can never stall or corrupt.
 *
 * The header stays windows.h-free: `device` is ID3D12Device*,
 * `command_list` is ID3D12GraphicsCommandList*, and buffer() returns
 * ID3D12Resource* for the embedder's raw SRV (R32_TYPELESS,
 * D3D12_BUFFER_SRV_FLAG_RAW).
 */
#ifndef PS3RECOMP_RSX_GPU_MIRROR_D3D12_H
#define PS3RECOMP_RSX_GPU_MIRROR_D3D12_H

#include "rsx_gpu_mirror.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_gpu_mirror_d3d12 rsx_gpu_mirror_d3d12;

/* Creates the space buffers (sizes rounded up to 16) and the staging area
 * (rounded up to three 4 KiB-aligned slices).  Returns NULL on failure. */
rsx_gpu_mirror_d3d12* rsx_gpu_mirror_d3d12_create(void* device,
                                                  u32 local_size,
                                                  u32 main_size,
                                                  u32 staging_bytes);
void rsx_gpu_mirror_d3d12_destroy(rsx_gpu_mirror_d3d12* b);

/* Guest resolver the upload path reads from (the runtime's vm map / gcm
 * carve, or a test arena). */
void rsx_gpu_mirror_d3d12_set_guest(
    rsx_gpu_mirror_d3d12* b,
    const u8* (*resolver)(void* user, u32 space, u32 offset, u32 min_bytes),
    void* user);

/* Fill the ops for rsx_gpu_mirror_create (user = the backend). */
void rsx_gpu_mirror_d3d12_get_ops(rsx_gpu_mirror_d3d12* b,
                                  rsx_gpu_mirror_ops* out);

int  rsx_gpu_mirror_d3d12_begin(rsx_gpu_mirror_d3d12* b, void* command_list);
/* Begin or resume the upload slice owned by one not-yet-submitted command
 * list. Repeated calls with the same list/submission fence append staging
 * bytes instead of rotating onto another fence-gated slice. */
int  rsx_gpu_mirror_d3d12_begin_fenced(rsx_gpu_mirror_d3d12* b,
                                       void* command_list,
                                       u64 submission_fence);
void rsx_gpu_mirror_d3d12_end(rsx_gpu_mirror_d3d12* b, u64 fence_value);
void rsx_gpu_mirror_d3d12_retire(rsx_gpu_mirror_d3d12* b,
                                 u64 completed_fence_value);

/* Record a later exact-byte patch into the current mirror session.  The
 * bytes are copied into fence-owned staging and compared once with the guest
 * source before the GPU copy is recorded.  This is the narrow fallback for
 * a required draw span whose surrounding 1 KiB tracking page is being
 * modified for a future draw; it never marks that whole page current. */
int rsx_gpu_mirror_d3d12_patch_exact(rsx_gpu_mirror_d3d12* b, u32 space,
                                     u32 offset, const u8* src, u32 size);

/* ID3D12Resource* of a space's mirror buffer (NULL for an empty space),
 * and its byte size (the value to put in the pull cbuffer mem_size). */
void* rsx_gpu_mirror_d3d12_buffer(rsx_gpu_mirror_d3d12* b, u32 space);
u32   rsx_gpu_mirror_d3d12_buffer_size(rsx_gpu_mirror_d3d12* b, u32 space);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_GPU_MIRROR_D3D12_H */
