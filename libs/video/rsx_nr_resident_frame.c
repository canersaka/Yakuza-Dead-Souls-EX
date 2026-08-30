#include "rsx_nr_resident_frame.h"

#include <string.h>

void rsx_nr_resident_frame_init(rsx_nr_resident_frame* state, int enabled)
{
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->enabled = enabled ? 1u : 0u;
}

uint64_t rsx_nr_resident_frame_submit(rsx_nr_resident_frame* state,
                                      uint64_t submitted_fence)
{
    if (!state || !state->enabled || !submitted_fence)
        return submitted_fence;
    state->allocator_fence[state->allocator_slot] = submitted_fence;
    state->allocator_slot =
        (state->allocator_slot + 1u) % RSX_NR_RESIDENT_ALLOCATORS;
    return state->allocator_fence[state->allocator_slot];
}

int rsx_nr_resident_frame_can_recycle(uint64_t recording_fence,
                                      uint64_t completed_fence)
{
    return recording_fence && completed_fence >= recording_fence;
}
