/*
 * ps3recomp - cellRemotePlay HLE
 *
 * Remote Play for PS Vita / PSP streaming.
 * Stub — init/end lifecycle, always reports not available.
 */

#ifndef PS3RECOMP_CELL_REMOTE_PLAY_H
#define PS3RECOMP_CELL_REMOTE_PLAY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80029601-04) were invented and collided byte-for-byte
 * with SCE_NP_COMMERCE_BROWSE_SERVER_ERROR_*. Real cellRemotePlay
 * (sysutil_remoteplay.h) documents exactly one error code, INTERNAL; the
 * only other real remoteplay-facility code is the obsolete ZERO_REGISTERED
 * alias recorded in sysutil_subdisplay.h. No init-lifecycle or param-error
 * codes exist for this module in the real SDK.
 */
#define CELL_REMOTEPLAY_ERROR_INTERNAL        0x80029830  /* real SDK value (sysutil_remoteplay.h:25) */
#define CELL_REMOTEPLAY_ERROR_ZERO_REGISTERED 0x80029813  /* real SDK value, obsolete remoteplay alias (sysutil_subdisplay.h:28) */

#define CELL_REMOTE_PLAY_ERROR_NOT_INITIALIZED     CELL_REMOTEPLAY_ERROR_INTERNAL         /* alias: no real lifecycle code; sole generic real code */
#define CELL_REMOTE_PLAY_ERROR_ALREADY_INITIALIZED CELL_REMOTEPLAY_ERROR_INTERNAL         /* alias: same */
#define CELL_REMOTE_PLAY_ERROR_INVALID_ARGUMENT    CELL_REMOTEPLAY_ERROR_INTERNAL         /* alias: no real param-error code; generic fallback */
#define CELL_REMOTE_PLAY_ERROR_NOT_SUPPORTED       CELL_REMOTEPLAY_ERROR_ZERO_REGISTERED  /* alias: closest real semantic (no peer registered = feature inoperative), matches our always-unavailable stub */

/* Functions */
s32 cellRemotePlayInit(void);
s32 cellRemotePlayEnd(void);
s32 cellRemotePlayIsAvailable(void);
s32 cellRemotePlayGetStatus(u32* status);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_REMOTE_PLAY_H */
