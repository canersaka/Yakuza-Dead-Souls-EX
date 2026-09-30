/*
 * ps3recomp - cellVideoOut HLE implementation
 *
 * Video output configuration: resolution, display mode, device info.
 * Defaults to 1280x720 (720p).
 */

#include "cellVideoOut.h"
#include "ps3emu/endian.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static u8  s_resolution_id  = CELL_VIDEO_OUT_RESOLUTION_720;
static u8  s_color_format   = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
static u8  s_aspect         = CELL_VIDEO_OUT_ASPECT_16_9;
static u32 s_pitch          = 1280 * 4;

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static void get_resolution_wh(u32 resId, u16* w, u16* h)
{
    switch (resId) {
    case CELL_VIDEO_OUT_RESOLUTION_1080:
        *w = 1920; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_720:
        *w = 1280; *h = 720;  break;
    case CELL_VIDEO_OUT_RESOLUTION_480:
        *w = 720;  *h = 480;  break;
    case CELL_VIDEO_OUT_RESOLUTION_576:
        *w = 720;  *h = 576;  break;
    case CELL_VIDEO_OUT_RESOLUTION_1600x1080:
        *w = 1600; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_1440x1080:
        *w = 1440; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_1280x1080:
        *w = 1280; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_960x1080:
        *w = 960;  *h = 1080; break;
    default:
        *w = 1280; *h = 720;  break;
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state)
{
    if (!state)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    /* Only the primary output is supported; SECONDARY is rejected like every
     * sibling entry point. */
    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    memset(state, 0, sizeof(CellVideoOutState));

    /* ENABLED is 0 (2 is PREPARING, in which no other member is valid), and
     * a spec-following boot polls for ENABLED. displayMode is the 8-byte
     * struct, not a packed u32, and at least one refreshRates bit must be
     * set in the mode returned here. */
    state->state      = CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED;
    state->colorSpace = CELL_VIDEO_OUT_COLOR_SPACE_RGB;
    state->displayMode.resolutionId = (u8)s_resolution_id;
    state->displayMode.scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    state->displayMode.conversion   = 0;   /* CONVERSION_NONE */
    state->displayMode.aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    state->displayMode.refreshRates =
        ps3_bswap16((s_resolution_id == CELL_VIDEO_OUT_RESOLUTION_576)
                        ? CELL_VIDEO_OUT_REFRESH_RATE_50HZ
                        : CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);

    return CELL_OK;
}

s32 cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution)
{
    if (!resolution)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    /* The out-struct lives in guest memory: fields are big-endian (a
     * host-endian write would hand the game byte-swapped width/height). */
    u16 w, h;
    get_resolution_wh(resolutionId, &w, &h);
    resolution->width  = ps3_bswap16(w);
    resolution->height = ps3_bswap16(h);

    return CELL_OK;
}

s32 cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config,
                            void* option, u32 waitForEvent)
{
    if (!config)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    s_resolution_id = config->resolutionId;
    s_color_format  = config->format;
    s_aspect        = config->aspect;

    /* config is a guest BE struct: byte-swap multi-byte reads */
    if (ps3_bswap32(config->pitch) > 0) {
        s_pitch = ps3_bswap32(config->pitch);
    } else {
        u16 w, h;
        get_resolution_wh(s_resolution_id, &w, &h);
        s_pitch = (u32)w * 4;
    }

    return CELL_OK;
}

s32 cellVideoOutGetConfiguration(u32 videoOut, CellVideoOutConfiguration* config,
                                   void* option)
{
    if (!config)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    memset(config, 0, sizeof(CellVideoOutConfiguration));
    config->resolutionId = s_resolution_id;
    config->format       = s_color_format;
    config->aspect       = s_aspect;
    config->pitch        = ps3_bswap32(s_pitch);  /* guest BE struct */

    return CELL_OK;
}

s32 cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex,
                               CellVideoOutDeviceInfo* info)
{
    if (!info)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    memset(info, 0, sizeof(CellVideoOutDeviceInfo));

    info->portType       = CELL_VIDEO_OUT_OUTPUT_HDMI;
    info->colorSpace     = CELL_VIDEO_OUT_COLOR_SPACE_RGB;
    info->latency        = 0;
    /* CellVideoOutDeviceState is UNAVAILABLE=0/AVAILABLE=1; any other value
     * is out of range and could read as "no display". */
    info->state          = 1; /* CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE */
    info->rgbOutputRange = 1;

    /* Report supported modes */
    int idx = 0;

    /* 480p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_480;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    info->availableModes[idx].refreshRates = ps3_bswap16(CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);
    idx++;

    /* 576p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_576;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    info->availableModes[idx].refreshRates = ps3_bswap16(CELL_VIDEO_OUT_REFRESH_RATE_50HZ);
    idx++;

    /* 720p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_720;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    info->availableModes[idx].refreshRates = ps3_bswap16(CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);
    idx++;

    /* 1080p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_1080;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    info->availableModes[idx].refreshRates = ps3_bswap16(CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ);
    idx++;

    info->availableModeCount = (u8)idx;

    return CELL_OK;
}

s32 cellVideoOutGetNumberOfDevice(u32 videoOut)
{
    if (videoOut == CELL_VIDEO_OUT_PRIMARY)
        return 1;

    return 0;
}
