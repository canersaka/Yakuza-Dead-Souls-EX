/*
 * ps3recomp - cellGameRecording HLE
 *
 * In-game video recording. Captures framebuffer output for replay/sharing.
 * Stub — init/end lifecycle, recording is a no-op (no framebuffer capture).
 */

#ifndef PS3RECOMP_CELL_GAME_RECORDING_H
#define PS3RECOMP_CELL_GAME_RECORDING_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes
 * Previous values (0x80610701-06) were invented and collided byte-for-byte
 * with CELL_SAIL_ERROR_INVALID_ARG (sail is imported/live). Re-pointed at
 * real cellRec values, base CELL_SYSUTIL_ERROR_BASE_REC (real SDK value;
 * sysutil_common.h:38, matches "cellGameRecording" == real "cellRec").
 * Real module has no "not supported" concept — closest generic real code
 * (FATAL) used instead.
 */
#define CELL_REC_ERROR_OUT_OF_MEMORY  0x8002c501  /* real SDK value (sysutil_rec.h:28) */
#define CELL_REC_ERROR_FATAL          0x8002c502  /* real SDK value (sysutil_rec.h:29) */
#define CELL_REC_ERROR_INVALID_VALUE  0x8002c503  /* real SDK value (sysutil_rec.h:30) */
#define CELL_REC_ERROR_INVALID_STATE  0x8002c506  /* real SDK value (sysutil_rec.h:33) */

#define CELL_GAME_REC_ERROR_NOT_INITIALIZED     CELL_REC_ERROR_INVALID_STATE  /* alias: init-check is a state-validity check */
#define CELL_GAME_REC_ERROR_ALREADY_INITIALIZED CELL_REC_ERROR_INVALID_STATE  /* alias: same state-domain code */
#define CELL_GAME_REC_ERROR_INVALID_ARGUMENT    CELL_REC_ERROR_INVALID_VALUE  /* alias: closest real semantic (invalid value) */
#define CELL_GAME_REC_ERROR_OUT_OF_MEMORY       CELL_REC_ERROR_OUT_OF_MEMORY  /* alias: exact real semantic */
#define CELL_GAME_REC_ERROR_NOT_SUPPORTED       CELL_REC_ERROR_FATAL          /* alias: no real not-supported concept; generic fallback */
#define CELL_GAME_REC_ERROR_BUSY                CELL_REC_ERROR_INVALID_STATE  /* alias: recording-already-active is a state conflict */

/* Recording quality */
#define CELL_GAME_REC_QUALITY_LOW     0
#define CELL_GAME_REC_QUALITY_MEDIUM  1
#define CELL_GAME_REC_QUALITY_HIGH    2

/* Recording parameters */
typedef struct CellGameRecordingParam {
    u32 quality;
    u32 maxDuration; /* seconds */
    u32 width;
    u32 height;
    u32 fps;
    u32 flags;
    u32 reserved[4];
} CellGameRecordingParam;

/* Functions */
s32 cellGameRecordingInit(const CellGameRecordingParam* param);
s32 cellGameRecordingEnd(void);
s32 cellGameRecordingStart(void);
s32 cellGameRecordingStop(void);
s32 cellGameRecordingPause(void);
s32 cellGameRecordingResume(void);
s32 cellGameRecordingIsRecording(void);
s32 cellGameRecordingGetDuration(float* duration);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GAME_RECORDING_H */
