#ifndef RSX_NR_RESIDENT_FRAME_H
#define RSX_NR_RESIDENT_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NR_RESIDENT_ALLOCATORS 4u

typedef struct rsx_nr_resident_frame {
    uint64_t allocator_fence[RSX_NR_RESIDENT_ALLOCATORS];
    uint32_t allocator_slot;
    uint32_t enabled;
} rsx_nr_resident_frame;

void rsx_nr_resident_frame_init(rsx_nr_resident_frame* state, int enabled);
uint64_t rsx_nr_resident_frame_submit(rsx_nr_resident_frame* state,
                                      uint64_t submitted_fence);
int rsx_nr_resident_frame_can_recycle(uint64_t recording_fence,
                                      uint64_t completed_fence);

#ifdef __cplusplus
}
#endif

#endif
