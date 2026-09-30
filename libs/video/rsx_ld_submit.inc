/* Command list submission and GPU waits.
 * Part of rsx_live_draw.c, which includes its parts once, in order. */

#include "rsx_legacy_frame.h"

static void ld_flush_mode(ld_flush_reason reason, int allow_async)
{
    const HRESULT close_hr = g.list->lpVtbl->Close(g.list);
    if (FAILED(close_hr)) {
        fprintf(stderr,
                "[d3d-fail] command-list Close hr=0x%08lX frame=%u\n",
                (unsigned long)close_hr, g_ld_frames);
        /* Failure-only diagnostics: when the optional D3D validation layer
         * is enabled, preserve the exact command-list error that made Close
         * fail.  Without the debug layer there is no info queue and this costs only
         * this already-failing branch. */
        ld_drain_info_queue("close-fail");
        ld_dump_dred("Close", close_hr);
        g.ready = 0;
        return;
    }
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    const u64 v = ++g.fence_value;
    const HRESULT signal_hr =
        g.queue->lpVtbl->Signal(g.queue, g.fence, v);
    if (FAILED(signal_hr)) {
        fprintf(stderr,
                "[d3d-fail] queue Signal hr=0x%08lX frame=%u\n",
                (unsigned long)signal_hr, g_ld_frames);
        ld_dump_dred("Signal", signal_hr);
        g.ready = 0;
        return;
    }
    const int asynchronous = allow_async == 2;
    if (!asynchronous &&
        g.fence->lpVtbl->GetCompletedValue(g.fence) < v) {
        const HRESULT event_hr =
            g.fence->lpVtbl->SetEventOnCompletion(
                g.fence, v, g.fence_event);
        if (FAILED(event_hr)) {
            fprintf(stderr,
                    "[d3d-fail] fence SetEventOnCompletion "
                    "hr=0x%08lX frame=%u\n",
                    (unsigned long)event_hr, g_ld_frames);
            ld_dump_dred("SetEventOnCompletion", event_hr);
            g.ready = 0;
            return;
        }
        WaitForSingleObject(g.fence_event, INFINITE);
    }
    ld_drain_info_queue("flush");
    /* Dynamic guest textures can replace a cached D3D resource while an
     * earlier draw in this command list still references the old one.  The
     * fence above is the first safe point at which those old resources may be
     * released. */
    if (!asynchronous) {
        for (u32 i = 0; i < g.n_retired_textures; i++)
            g.retired_textures[i]->lpVtbl->Release(g.retired_textures[i]);
        g.n_retired_textures = 0;
        g.alloc->lpVtbl->Reset(g.alloc);
        g.list->lpVtbl->Reset(g.list, g.alloc, NULL);
        g.upload_used = 0;
    } else {
        if (!ld_frame_rotate(v, (u32)reason)) {
            fprintf(stderr, "[d3d-fail] legacy frame reuse failed fence=%llu\n",
                    (unsigned long long)v);
            g.ready = 0;
            return;
        }
    }
    g.list_generation++;
}

static void ld_flush(ld_flush_reason reason)
{
    ld_flush_mode(reason, 0);
}


/* Public wrapper for the RSX SET_REFERENCE / sync fence: block until the GPU
 * has finished all queued draws, so the game's REF poll advances only after
 * the GPU has really caught up. Without it our async consumer writes REF
 * instantly and races ahead of real GPU time. The capture replay calls it at
 * each reference point. */
void rsx_live_draw_flush(void)
{
    if (g.ready) ld_flush(LD_FLUSH_GUEST_REFERENCE);
}

static void retire_texture(ID3D12Resource* tex)
{
    if (!tex) return;
    if (g.n_retired_textures >= MAX_RETIRED_TEXTURES)
        ld_flush(LD_FLUSH_RETIRE_QUEUE);
    g.retired_textures[g.n_retired_textures++] = tex;
}
