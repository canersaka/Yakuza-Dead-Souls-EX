#ifndef PS3RECOMP_SPU_JOB_DISPATCH_H
#define PS3RECOMP_SPU_JOB_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A SPURS job binary runs wherever the scheduler places it in local store,
 * and each (binary, base) pair is lifted separately. The game registers its
 * placement table once at startup; a job launch uses it to pick the image id
 * of the lift for its exact placement.
 */
typedef struct spu_job_placement {
    uint32_t binary_ea;
    uint32_t base;
    int image;
} spu_job_placement;

void spu_job_placements_set(const spu_job_placement* table, unsigned count);

/* Image id of the lift for a job binary loaded at `base`, or -1. */
int spu_job_placement_image(uint32_t binary_ea, uint32_t base);

#ifdef __cplusplus
}
#endif

#endif
