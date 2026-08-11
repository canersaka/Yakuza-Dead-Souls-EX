/*
 * ps3recomp - cellOvis HLE
 *
 * System overlay notifications. Stub — init/term work,
 * overlay operations are no-ops.
 */

#ifndef PS3RECOMP_CELL_OVIS_H
#define PS3RECOMP_CELL_OVIS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80410701-03) were invented and collided byte-for-byte
 * with CELL_SPURS_CORE_ERROR_*. Re-pointed at real cellOvis values
 * (cell/ovis/error.h). Real cellOvis documents only two codes (INVAL,
 * ALIGN) and has no init-lifecycle concept at all (SDK exposes only
 * InitializeOverlayTable/GetOverlayTableSize, no explicit init/term) — all
 * three of our lifecycle-check constants collapse onto the closest real
 * code, INVAL, since ALIGN's alignment-specific semantic fits none of them.
 */
#define CELL_OVIS_ERROR_INVAL  0x80410402  /* real SDK value (cell/ovis/error.h:22) */
#define CELL_OVIS_ERROR_ALIGN  0x80410416  /* real SDK value (cell/ovis/error.h:25), unused by our stub but recorded for completeness */

#define CELL_OVIS_ERROR_NOT_INITIALIZED     CELL_OVIS_ERROR_INVAL  /* alias: no real lifecycle code; closest generic real code */
#define CELL_OVIS_ERROR_ALREADY_INITIALIZED CELL_OVIS_ERROR_INVAL  /* alias: same */
#define CELL_OVIS_ERROR_INVALID_ARGUMENT    CELL_OVIS_ERROR_INVAL  /* alias: exact real semantic */

/* Types */
typedef u32 CellOvisHandle;

/* Functions */
s32 cellOvisInit(void);
s32 cellOvisTerm(void);

s32 cellOvisGetOverlayTableSize(const char* filePath, u32* tableSize);
s32 cellOvisCreateOverlay(const void* table, u32 tableSize, CellOvisHandle* handle);
s32 cellOvisDestroyOverlay(CellOvisHandle handle);

s32 cellOvisInvalidateOverlay(CellOvisHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_OVIS_H */
