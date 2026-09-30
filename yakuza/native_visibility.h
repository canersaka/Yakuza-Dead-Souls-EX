#ifndef YZ_NATIVE_VISIBILITY_H
#define YZ_NATIVE_VISIBILITY_H
#include <stdint.h>
#include <stddef.h>
/* Engine visibility operation, independent of SPU registers, local store, DMA
 * channels and RSX commands. Resource bytes are currently the engine's BE layout.
 * An oriented bound is 64 bytes: header, center/size, extents, quaternion. */
typedef struct yz_visibility_request {
    const uint8_t* object_matrix; /* four 16-byte columns */
    const uint8_t* bounds;
    const uint8_t* camera;        /* planes, view transform and occluder arrays */
    size_t camera_bytes;
    uint32_t count;
    uint32_t far_limit_bits;
} yz_visibility_request;
typedef struct yz_visibility_result {
    uint8_t visible[128];
    uint32_t depth_bits[128];     /* host endian bits; zero when invisible */
} yz_visibility_result;
/* Reject unsupported sizes before changing output. Numeric results follow the
 * SPU's truncation and extended finite range. */
int yz_visibility_oriented(const yz_visibility_request*, yz_visibility_result*);
#endif
