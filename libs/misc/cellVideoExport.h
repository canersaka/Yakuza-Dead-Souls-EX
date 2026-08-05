/*
 * ps3recomp - cellVideoExport HLE
 *
 * Video export utility for saving game-captured video to the XMB video column.
 * Stub — init/end lifecycle, export returns NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_VIDEO_EXPORT_H
#define PS3RECOMP_CELL_VIDEO_EXPORT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80610501-06) were invented and collided byte-for-byte
 * with CELL_PAMF_ERROR_STREAM_NOT_FOUND's range (movie path is live).
 * Re-pointed at real cellVideoExport values, base
 * CELL_SYSUTIL_ERROR_BASE_VIDEO_EXPORT_UTIL (sysutil_common.h:43). Real
 * module has no OOM or not-supported concept — closest documented real
 * codes used instead.
 */
#define CELL_VIDEO_EXPORT_UTIL_ERROR_BUSY      0x8002ca01  /* real SDK value (sysutil_video_export.h:22) */
#define CELL_VIDEO_EXPORT_UTIL_ERROR_INTERNAL  0x8002ca02  /* real SDK value (sysutil_video_export.h:23) */
#define CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM     0x8002ca03  /* real SDK value (sysutil_video_export.h:24) */
#define CELL_VIDEO_EXPORT_UTIL_ERROR_ACCESS_ERROR 0x8002ca04  /* real SDK value (sysutil_video_export.h:25) */
#define CELL_VIDEO_EXPORT_UTIL_ERROR_INITIALIZE   0x8002ca0a  /* real SDK value (sysutil_video_export.h:31) */

#define CELL_VIDEO_EXPORT_ERROR_NOT_INITIALIZED     CELL_VIDEO_EXPORT_UTIL_ERROR_INITIALIZE   /* alias: init-domain real code */
#define CELL_VIDEO_EXPORT_ERROR_ALREADY_INITIALIZED CELL_VIDEO_EXPORT_UTIL_ERROR_INTERNAL      /* alias: no real "already initialized" concept; generic fallback */
#define CELL_VIDEO_EXPORT_ERROR_INVALID_ARGUMENT    CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM          /* alias: exact real semantic */
#define CELL_VIDEO_EXPORT_ERROR_OUT_OF_MEMORY       CELL_VIDEO_EXPORT_UTIL_ERROR_ACCESS_ERROR   /* alias: no real OOM code; closest storage-resource-exhaustion analog */
#define CELL_VIDEO_EXPORT_ERROR_NOT_SUPPORTED       CELL_VIDEO_EXPORT_UTIL_ERROR_INTERNAL       /* alias: no real not-supported concept; generic fallback */
#define CELL_VIDEO_EXPORT_ERROR_BUSY                CELL_VIDEO_EXPORT_UTIL_ERROR_BUSY           /* alias: exact real semantic */

/* Export format */
#define CELL_VIDEO_EXPORT_FORMAT_AVI    0
#define CELL_VIDEO_EXPORT_FORMAT_MP4    1

/* Export parameters */
typedef struct CellVideoExportParam {
    const char* filePath;
    const char* title;
    const char* gameTitle;
    u32 format;
    u32 flags;
    u32 reserved[4];
} CellVideoExportParam;

/* Callback */
typedef void (*CellVideoExportCallback)(s32 result, void* userdata);

/* Functions */
s32 cellVideoExportInit(void);
s32 cellVideoExportEnd(void);
s32 cellVideoExportStart(const CellVideoExportParam* param,
                          CellVideoExportCallback callback, void* userdata);
s32 cellVideoExportAbort(void);
s32 cellVideoExportGetProgress(float* progress);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VIDEO_EXPORT_H */
