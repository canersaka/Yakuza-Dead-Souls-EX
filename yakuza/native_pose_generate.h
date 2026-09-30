#ifndef YAKUZA_NATIVE_POSE_GENERATE_H
#define YAKUZA_NATIVE_POSE_GENERATE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Result of the scene pose job (job binary 0x01254500), with the SPU's
 * fused/truncating extended-range numeric rules. Model matrices are required.
 * No PS3 state or job/DMA emulation. Only the nominal count is read/written;
 * the SPU job's four-bone padding never belongs to the published pose.
 * Preserves the caller's floating environment. */
int yz_pose_generate_spu48(void* output,size_t output_bytes,
    const void* world64,size_t world_bytes,const void* bones64,size_t bone_bytes,
    const void* model64,size_t model_bytes,uint32_t count);
#ifdef __cplusplus
}
#endif
#endif
