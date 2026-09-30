/*
 * ps3recomp - Fundamental PS3 types
 *
 * Fixed-width integers, the 128-bit vector type and guest handle types.
 */

#ifndef PS3EMU_PS3TYPES_H
#define PS3EMU_PS3TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * C-visible fixed-width typedefs
 * -----------------------------------------------------------------------*/
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uint8_t   b8; /* PS3 boolean (1 byte) */

/* 128-bit types for VMX / AltiVec registers.
 * Kept as a union so the same object can be accessed by lane width. */
typedef union u128_t {
    uint64_t _u64[2];
    uint32_t _u32[4];
    uint16_t _u16[8];
    uint8_t  _u8[16];
    int64_t  _s64[2];
    int32_t  _s32[4];
    int16_t  _s16[8];
    int8_t   _s8[16];
    float    _f32[4];
    double   _f64[2];
#ifdef __cplusplus
    /* Convenience: construct zero-initialized */
    u128_t() : _u64{} {}
    u128_t(uint64_t hi, uint64_t lo) : _u64{hi, lo} {}
#endif
} u128;

/* Guest handle types */
typedef u64  sys_ppu_thread_t;

#ifdef __cplusplus
/* C++ translation units get ps3_bswap* through this header. */
#include "endian.h"
#endif

#endif /* PS3EMU_PS3TYPES_H */
