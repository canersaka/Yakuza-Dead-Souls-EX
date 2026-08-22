#ifndef SPU_XFLOAT_SIMD_H
#define SPU_XFLOAT_SIMD_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set once after the OS/CPU check.  The workload entry calls
 * spu_xfloat_simd_thread_enter() before generated SPU code can observe it. */
extern volatile int g_spu_xfloat_simd_enabled;

/* Establish the per-host-thread SPU rounding mode and return whether the
 * packed ordinary-lane adapter is usable on this machine. */
int spu_xfloat_simd_thread_enter(void);

/* Return one only when every input and output lane is representable by the
 * ordinary IEEE-shaped subset of SPU xfloat.  A zero return requires the
 * caller to execute the scalar extended-range reference path. */
int spu_xfloat_simd_fa(u128* out, const u128* a, const u128* b);
int spu_xfloat_simd_fs(u128* out, const u128* a, const u128* b);
int spu_xfloat_simd_fm(u128* out, const u128* a, const u128* b);
int spu_xfloat_simd_fma(u128* out, const u128* a, const u128* b, const u128* c);
int spu_xfloat_simd_fms(u128* out, const u128* a, const u128* b, const u128* c);
int spu_xfloat_simd_fnms(u128* out, const u128* a, const u128* b, const u128* c);

#ifdef __cplusplus
}
#endif

#endif
