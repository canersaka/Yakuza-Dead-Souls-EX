/*
 * ps3recomp - cellSubdisplay HLE
 *
 * Sub-display output for PS Vita Remote Play and similar second-screen features.
 * Stub — init/end lifecycle, video/touch data exchange all succeed as no-ops.
 */

#ifndef PS3RECOMP_CELL_SUBDISPLAY_H
#define PS3RECOMP_CELL_SUBDISPLAY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80029501-06) were invented and collided byte-for-byte
 * with SCE_NP_DRM_ERROR_OUT_OF_MEMORY/INVALID_PARAM (NP DRM is live).
 * Re-pointed at real cellSubDisplay values (sysutil_subdisplay.h). Most of
 * our names already match the real SDK names exactly; only the values were
 * wrong. Two have no real analog and use the closest documented real code.
 */
#define CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED     0x80029855  /* real SDK value (sysutil_subdisplay.h:18), exact name match */
#define CELL_SUBDISPLAY_ERROR_OUT_OF_MEMORY       0x80029851  /* real SDK value (sysutil_subdisplay.h:14), exact name match */
#define CELL_SUBDISPLAY_ERROR_NOT_SUPPORTED       0x80029856  /* real SDK value (sysutil_subdisplay.h:19), exact name match */
#define CELL_SUBDISPLAY_ERROR_FATAL               0x80029852  /* real SDK value (sysutil_subdisplay.h:15) */
#define CELL_SUBDISPLAY_ERROR_INVALID_VALUE       0x80029854  /* real SDK value (sysutil_subdisplay.h:17) */
#define CELL_SUBDISPLAY_ERROR_ZERO_REGISTERED     0x80029813  /* real SDK value (sysutil_subdisplay.h:25) */

#define CELL_SUBDISPLAY_ERROR_ALREADY_INITIALIZED CELL_SUBDISPLAY_ERROR_FATAL           /* alias: no real "already initialized" concept; generic fallback */
#define CELL_SUBDISPLAY_ERROR_INVALID_ARGUMENT    CELL_SUBDISPLAY_ERROR_INVALID_VALUE    /* alias: closest real semantic (invalid value) */
#define CELL_SUBDISPLAY_ERROR_NOT_CONNECTED       CELL_SUBDISPLAY_ERROR_ZERO_REGISTERED  /* alias: closest real semantic ("num of registered PSP is zero") */

/* Video mode */
#define CELL_SUBDISPLAY_VIDEO_MODE_DEFAULT   0
#define CELL_SUBDISPLAY_VIDEO_MODE_480P      1
#define CELL_SUBDISPLAY_VIDEO_MODE_272P      2

/* Config */
typedef struct CellSubdisplayConfig {
    u32 videoMode;
    u32 width;
    u32 height;
    u32 fps;
    u32 flags;
    u32 reserved[4];
} CellSubdisplayConfig;

/* Touch data from sub-display */
typedef struct CellSubdisplayTouchData {
    u32 count;
    struct {
        u16 x;
        u16 y;
        u16 pressure;
        u16 reserved;
    } points[4];
} CellSubdisplayTouchData;

/* Functions */
s32 cellSubdisplayInit(const CellSubdisplayConfig* config);
s32 cellSubdisplayEnd(void);
s32 cellSubdisplayStart(void);
s32 cellSubdisplayStop(void);
s32 cellSubdisplayGetRequiredMemory(u32* size);
s32 cellSubdisplayGetTouchData(CellSubdisplayTouchData* data);
s32 cellSubdisplayIsConnected(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SUBDISPLAY_H */
