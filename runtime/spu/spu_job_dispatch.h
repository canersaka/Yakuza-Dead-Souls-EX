#ifndef PS3RECOMP_SPU_JOB_DISPATCH_H
#define PS3RECOMP_SPU_JOB_DISPATCH_H

#include <stdint.h>

/*
 * Resolve job binaries whose launch descriptor is more authoritative than
 * the per-SPU residency cache.  This must not depend on a scene/diagnostic
 * flag: SPURS can launch the descriptor before higher-level scene tracking
 * observes the corresponding asset open.
 */
static inline int spu_job_descriptor_image(
    int current_image, uint32_t entry_pc, uint32_t binary_ea)
{
    if (current_image == 13 &&
        entry_pc == 0x04C00u &&
        binary_ea == 0x01252680u)
        return 19;

    return -1;
}

#endif
