/*
 * ps3recomp - cellBGDL HLE
 *
 * Background download manager for DLC/patches.
 * Stub — init/term work, no downloads occur.
 */

#ifndef PS3RECOMP_CELL_BGDL_H
#define PS3RECOMP_CELL_BGDL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80410901-05) were invented and collided byte-for-byte
 * with CELL_SPURS_TASK_ERROR_*. Re-pointed at real cellBGDL values, base
 * CELL_SYSUTIL_ERROR_BASE_BGDL (real SDK value; sysutil_common.h:48).
 * Real cellBGDL has no init-lifecycle codes (SDK exposes only SetMode/
 * GetMode) — NOT_INITIALIZED/ALREADY_INITIALIZED have no exact analog and
 * fall back to the closest documented real code.
 */
#define CELL_BGDL_UTIL_ERROR_BUSY          0x8002ce01  /* real SDK value (sysutil_bgdl.h:21) */
#define CELL_BGDL_UTIL_ERROR_INTERNAL      0x8002ce02  /* real SDK value (sysutil_bgdl.h:22) */
#define CELL_BGDL_UTIL_ERROR_PARAM         0x8002ce03  /* real SDK value (sysutil_bgdl.h:23) */
#define CELL_BGDL_UTIL_ERROR_ACCESS_ERROR  0x8002ce04  /* real SDK value (sysutil_bgdl.h:24) */
#define CELL_BGDL_UTIL_ERROR_INITIALIZE    0x8002ce05  /* real SDK value (sysutil_bgdl.h:25) */

#define CELL_BGDL_ERROR_NOT_INITIALIZED     CELL_BGDL_UTIL_ERROR_INITIALIZE    /* alias: closest real code is init-domain (no NOT_INITIALIZED concept in real SDK) */
#define CELL_BGDL_ERROR_ALREADY_INITIALIZED CELL_BGDL_UTIL_ERROR_INTERNAL      /* alias: no real "already initialized" concept; generic fallback */
#define CELL_BGDL_ERROR_INVALID_ARGUMENT    CELL_BGDL_UTIL_ERROR_PARAM         /* alias: exact real semantic (invalid parameter) */
#define CELL_BGDL_ERROR_NOT_FOUND           CELL_BGDL_UTIL_ERROR_ACCESS_ERROR  /* alias: closest real code (HDD access error) */
#define CELL_BGDL_ERROR_BUSY                CELL_BGDL_UTIL_ERROR_BUSY          /* alias: exact real semantic */

/* Download status */
#define CELL_BGDL_STATUS_IDLE         0
#define CELL_BGDL_STATUS_DOWNLOADING  1
#define CELL_BGDL_STATUS_PAUSED       2
#define CELL_BGDL_STATUS_COMPLETE     3
#define CELL_BGDL_STATUS_ERROR        4

/* Types */
typedef u32 CellBgdlId;

typedef struct CellBgdlInfo {
    CellBgdlId id;
    s32 status;
    u64 totalSize;
    u64 downloadedSize;
    char contentId[48];
    u32 reserved[4];
} CellBgdlInfo;

/* Functions */
s32 cellBgdlInit(void);
s32 cellBgdlTerm(void);

s32 cellBgdlGetInfo(CellBgdlId id, CellBgdlInfo* info);
s32 cellBgdlGetList(CellBgdlInfo* list, u32 maxEntries, u32* numEntries);

s32 cellBgdlStartDownload(const char* url, const char* contentId,
                            CellBgdlId* id);
s32 cellBgdlPauseDownload(CellBgdlId id);
s32 cellBgdlResumeDownload(CellBgdlId id);
s32 cellBgdlCancelDownload(CellBgdlId id);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_BGDL_H */
