/*
 * ps3recomp - D3D12 backend for the persistent GPU guest-memory mirror
 *
 * See rsx_gpu_mirror_d3d12.h for the session/fence contract.  Windows-only;
 * a stub keeps other builds linking, mirroring rsx_live_draw.c.
 */
#include "rsx_gpu_mirror_d3d12.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>   /* selectany IIDs, same pattern as rsx_live_draw.c */
#include <d3d12.h>

#include <stdlib.h>
#include <string.h>

#define GMB_SLICES 3u

typedef struct gmb_space {
    ID3D12Resource* buffer;      /* DEFAULT heap, raw SRV target */
    u32 size;
    int in_copy_state;           /* COPY_DEST vs NON_PIXEL_SHADER_RESOURCE */
    int touched;                 /* uploads recorded this session */
} gmb_space;

struct rsx_gpu_mirror_d3d12 {
    ID3D12Device* dev;
    gmb_space space[RSX_GUEST_NUM_SPACES];
    ID3D12Resource* staging;
    u8* staging_mapped;
    u32 slice_size;
    u64 slice_fence[GMB_SLICES]; /* fence value that must complete before reuse */
    u32 slice_used;
    u32 slice_index;
    u64 retired_fence;
    u64 session_counter;
    ID3D12GraphicsCommandList* append_list;
    u64 append_fence;
    int append_valid;
    ID3D12GraphicsCommandList* list;   /* borrowed inside begin/end */
    int in_session;
    int slice_available;
    const u8* (*guest)(void* user, u32 space, u32 offset, u32 min_bytes);
    void* guest_user;
};

static ID3D12Resource* gmb_create_buffer(ID3D12Device* dev, u64 size,
                                         D3D12_HEAP_TYPE heap,
                                         D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = NULL;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(
            dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
            &IID_ID3D12Resource, (void**)&res)))
        return NULL;
    return res;
}

rsx_gpu_mirror_d3d12* rsx_gpu_mirror_d3d12_create(void* device,
                                                  u32 local_size,
                                                  u32 main_size,
                                                  u32 staging_bytes)
{
    ID3D12Device* dev = (ID3D12Device*)device;
    if (!dev || !staging_bytes)
        return NULL;
    rsx_gpu_mirror_d3d12* b =
        (rsx_gpu_mirror_d3d12*)calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->dev = dev;
    const u32 sizes[RSX_GUEST_NUM_SPACES] = {local_size, main_size};
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++) {
        if (!sizes[i])
            continue;
        const u32 rounded = (sizes[i] + 15u) & ~15u;
        b->space[i].buffer = gmb_create_buffer(
            dev, rounded, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST);
        if (!b->space[i].buffer) {
            rsx_gpu_mirror_d3d12_destroy(b);
            return NULL;
        }
        b->space[i].size = rounded;
        b->space[i].in_copy_state = 1;
    }
    b->slice_size = ((staging_bytes / GMB_SLICES) + 4095u) & ~4095u;
    b->staging = gmb_create_buffer(
        dev, (u64)b->slice_size * GMB_SLICES, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!b->staging) {
        rsx_gpu_mirror_d3d12_destroy(b);
        return NULL;
    }
    D3D12_RANGE none = {0, 0};
    if (FAILED(b->staging->lpVtbl->Map(b->staging, 0, &none,
                                       (void**)&b->staging_mapped))) {
        rsx_gpu_mirror_d3d12_destroy(b);
        return NULL;
    }
    return b;
}

void rsx_gpu_mirror_d3d12_destroy(rsx_gpu_mirror_d3d12* b)
{
    if (!b)
        return;
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++)
        if (b->space[i].buffer)
            b->space[i].buffer->lpVtbl->Release(b->space[i].buffer);
    if (b->staging) {
        if (b->staging_mapped)
            b->staging->lpVtbl->Unmap(b->staging, 0, NULL);
        b->staging->lpVtbl->Release(b->staging);
    }
    free(b);
}

void rsx_gpu_mirror_d3d12_set_guest(
    rsx_gpu_mirror_d3d12* b,
    const u8* (*resolver)(void* user, u32 space, u32 offset, u32 min_bytes),
    void* user)
{
    if (!b)
        return;
    b->guest = resolver;
    b->guest_user = user;
}

static const u8* gmb_ops_guest_ptr(void* user, u32 space, u32 offset,
                                   u32 min_bytes)
{
    rsx_gpu_mirror_d3d12* b = (rsx_gpu_mirror_d3d12*)user;
    if (!b->guest)
        return NULL;
    return b->guest(b->guest_user, space, offset, min_bytes);
}

static int gmb_ops_upload(void* user, u32 space, u32 offset, const u8* src,
                          u32 size)
{
    rsx_gpu_mirror_d3d12* b = (rsx_gpu_mirror_d3d12*)user;
    if (!b->in_session || !b->slice_available || !b->list ||
        space >= RSX_GUEST_NUM_SPACES)
        return -1;
    gmb_space* s = &b->space[space];
    if (!s->buffer || (u64)offset + size > s->size)
        return -1;
    if (b->slice_used + (u64)size > b->slice_size)
        return -1;   /* slice exhausted: pages stay dirty and retry */
    const u32 pos = b->slice_index * b->slice_size + b->slice_used;
    memcpy(b->staging_mapped + pos, src, size);
    b->list->lpVtbl->CopyBufferRegion(b->list, s->buffer, offset,
                                      b->staging, pos, size);
    b->slice_used += size;
    s->touched = 1;
    return 0;
}

int rsx_gpu_mirror_d3d12_patch_exact(rsx_gpu_mirror_d3d12* b, u32 space,
                                     u32 offset, const u8* src, u32 size)
{
    if (!b || !src || !size || !b->in_session || !b->slice_available ||
        !b->list || space >= RSX_GUEST_NUM_SPACES)
        return -1;
    gmb_space* s = &b->space[space];
    if (!s->buffer || (u64)offset + size > s->size ||
        b->slice_used + (u64)size > b->slice_size)
        return -1;

    const u32 pos = b->slice_index * b->slice_size + b->slice_used;
    u8* const snapshot = b->staging_mapped + pos;
    memcpy(snapshot, src, size);
    MemoryBarrier();
    /* A post-copy equality check proves that the recorded bytes represent
     * one complete observed value of the exact shader-visible span.  Writes
     * elsewhere on its coarse tracking page do not invalidate that proof. */
    if (memcmp(snapshot, src, size) != 0)
        return -2;
    b->list->lpVtbl->CopyBufferRegion(b->list, s->buffer, offset,
                                      b->staging, pos, size);
    b->slice_used += size;
    s->touched = 1;
    return 0;
}

void rsx_gpu_mirror_d3d12_get_ops(rsx_gpu_mirror_d3d12* b,
                                  rsx_gpu_mirror_ops* out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->user = b;
    out->guest_ptr = gmb_ops_guest_ptr;
    out->upload = gmb_ops_upload;
}

static void gmb_transition(rsx_gpu_mirror_d3d12* b, gmb_space* s,
                           int to_copy)
{
    if (!s->buffer || s->in_copy_state == to_copy)
        return;
    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = s->buffer;
    bar.Transition.Subresource = 0;
    bar.Transition.StateBefore = to_copy
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_COPY_DEST;
    bar.Transition.StateAfter = to_copy
        ? D3D12_RESOURCE_STATE_COPY_DEST
        : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b->list->lpVtbl->ResourceBarrier(b->list, 1, &bar);
    s->in_copy_state = to_copy;
}

static int gmb_begin(rsx_gpu_mirror_d3d12* b, void* command_list,
                     u64 submission_fence, int allow_append)
{
    if (!b || !command_list || b->in_session)
        return -1;
    b->list = (ID3D12GraphicsCommandList*)command_list;
    b->in_session = 1;
    if (allow_append && b->append_valid &&
        b->append_list == b->list && b->append_fence == submission_fence) {
        /* Same command-list generation: all earlier copies and the draws
         * which consume them will retire under this fence. Keep appending to
         * its one slice; rotating per draw would exhaust all three slices
         * before this list is ever submitted. */
        b->slice_available = 1;
    } else {
        b->slice_index = (u32)(b->session_counter++ % GMB_SLICES);
        b->slice_used = 0;
        /* The slice is reusable only once the submission that last filled it
         * has completed. A different command-list generation may never
         * overwrite in-flight staging bytes. */
        b->slice_available =
            b->slice_fence[b->slice_index] <= b->retired_fence;
        b->append_list = allow_append ? b->list : NULL;
        b->append_fence = submission_fence;
        b->append_valid = allow_append;
    }
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++) {
        b->space[i].touched = 0;
        gmb_transition(b, &b->space[i], 1);
    }
    return 0;
}

int rsx_gpu_mirror_d3d12_begin(rsx_gpu_mirror_d3d12* b, void* command_list)
{
    return gmb_begin(b, command_list, 0, 0);
}

int rsx_gpu_mirror_d3d12_begin_fenced(rsx_gpu_mirror_d3d12* b,
                                      void* command_list,
                                      u64 submission_fence)
{
    if (!submission_fence)
        return -1;
    return gmb_begin(b, command_list, submission_fence, 1);
}

void rsx_gpu_mirror_d3d12_end(rsx_gpu_mirror_d3d12* b, u64 fence_value)
{
    if (!b || !b->in_session)
        return;
    for (u32 i = 0; i < RSX_GUEST_NUM_SPACES; i++)
        gmb_transition(b, &b->space[i], 0);
    if (b->slice_used)
        b->slice_fence[b->slice_index] = fence_value;
    b->list = NULL;
    b->in_session = 0;
}

void rsx_gpu_mirror_d3d12_retire(rsx_gpu_mirror_d3d12* b,
                                 u64 completed_fence_value)
{
    if (!b)
        return;
    if (completed_fence_value > b->retired_fence)
        b->retired_fence = completed_fence_value;
}

void* rsx_gpu_mirror_d3d12_buffer(rsx_gpu_mirror_d3d12* b, u32 space)
{
    if (!b || space >= RSX_GUEST_NUM_SPACES)
        return NULL;
    return b->space[space].buffer;
}

u32 rsx_gpu_mirror_d3d12_buffer_size(rsx_gpu_mirror_d3d12* b, u32 space)
{
    if (!b || space >= RSX_GUEST_NUM_SPACES)
        return 0;
    return b->space[space].size;
}

#else /* !_WIN32 */

rsx_gpu_mirror_d3d12* rsx_gpu_mirror_d3d12_create(void* device,
                                                  u32 local_size,
                                                  u32 main_size,
                                                  u32 staging_bytes)
{
    (void)device; (void)local_size; (void)main_size; (void)staging_bytes;
    return 0;
}
void rsx_gpu_mirror_d3d12_destroy(rsx_gpu_mirror_d3d12* b) { (void)b; }
void rsx_gpu_mirror_d3d12_set_guest(
    rsx_gpu_mirror_d3d12* b,
    const u8* (*resolver)(void* user, u32 space, u32 offset, u32 min_bytes),
    void* user)
{ (void)b; (void)resolver; (void)user; }
void rsx_gpu_mirror_d3d12_get_ops(rsx_gpu_mirror_d3d12* b,
                                  rsx_gpu_mirror_ops* out)
{ (void)b; if (out) { out->user = 0; out->guest_ptr = 0; out->upload = 0; } }
int rsx_gpu_mirror_d3d12_begin(rsx_gpu_mirror_d3d12* b, void* command_list)
{ (void)b; (void)command_list; return -1; }
int rsx_gpu_mirror_d3d12_begin_fenced(rsx_gpu_mirror_d3d12* b,
                                      void* command_list,
                                      u64 submission_fence)
{ (void)b; (void)command_list; (void)submission_fence; return -1; }
void rsx_gpu_mirror_d3d12_end(rsx_gpu_mirror_d3d12* b, u64 fence_value)
{ (void)b; (void)fence_value; }
void rsx_gpu_mirror_d3d12_retire(rsx_gpu_mirror_d3d12* b,
                                 u64 completed_fence_value)
{ (void)b; (void)completed_fence_value; }
int rsx_gpu_mirror_d3d12_patch_exact(rsx_gpu_mirror_d3d12* b, u32 space,
                                     u32 offset, const u8* src, u32 size)
{ (void)b; (void)space; (void)offset; (void)src; (void)size; return -1; }
void* rsx_gpu_mirror_d3d12_buffer(rsx_gpu_mirror_d3d12* b, u32 space)
{ (void)b; (void)space; return 0; }
u32 rsx_gpu_mirror_d3d12_buffer_size(rsx_gpu_mirror_d3d12* b, u32 space)
{ (void)b; (void)space; return 0; }

#endif /* _WIN32 */
