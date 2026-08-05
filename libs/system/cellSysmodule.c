/*
 * ps3recomp - cellSysmodule HLE implementation
 *
 * 2026-08-04 doc-conformance audit rework:
 *  - module-ID table regenerated from the SDK (cellSysmodule.h);
 *  - the eleven modules the firmware auto-loads at process start report
 *    loaded from the first call and survive UnloadModule (libsysmodule
 *    Overview p.9/p.12);
 *  - UnloadModule of a not-loaded module returns CELL_OK, the documented
 *    contract (Reference p.10) -- ERROR_UNLOADED belongs to IsLoaded only;
 *  - undocumented ids now return ERROR_UNKNOWN instead of being accepted;
 *  - all documented high-range (0xF0xx) ids are tracked, not just trophy.
 */

#include "cellSysmodule.h"
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Track which modules have been "loaded": dense table for ids < 0x0100 and
 * a second table for the documented 0xF0xx range. */
static int s_module_loaded[CELL_SYSMODULE_MAX_ID];
static int s_module_loaded_hi[0x100];   /* ids 0xF000..0xF0FF */

/* Every module id defined by the SDK (deduplicated aliases). */
static const u16 s_valid_ids[] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008,
    0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F, 0x0010, 0x0011,
    0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x001A,
    0x001B, 0x001C, 0x001D, 0x001E, 0x001F, 0x0020, 0x0021, 0x0022, 0x0023,
    0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C,
    0x002D, 0x002E, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036,
    0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048,
    0x0049, 0x004A, 0x004E, 0x004F, 0x0050, 0x0052, 0x0053, 0x0055, 0x0056,
    0x0057, 0x0059, 0x005A, 0x005C, 0xF00A, 0xF010, 0xF019, 0xF01B, 0xF01D,
    0xF01E, 0xF023, 0xF028, 0xF029, 0xF02A, 0xF02B, 0xF02C, 0xF02E, 0xF02F,
    0xF030, 0xF034, 0xF035, 0xF054
};

/* Modules the firmware loads at process activation; the application need
 * not load them and cannot unload them (libsysmodule Overview p.9, p.12;
 * Reference p.15). */
static const u16 s_autoloaded_ids[] = {
    CELL_SYSMODULE_SYSUTIL, CELL_SYSMODULE_GCM_SYS, CELL_SYSMODULE_AUDIO, CELL_SYSMODULE_IO, CELL_SYSMODULE_SYNC, CELL_SYSMODULE_SPURS, CELL_SYSMODULE_OVIS, CELL_SYSMODULE_SHEAP, CELL_SYSMODULE_DAISY, CELL_SYSMODULE_FS, CELL_SYSMODULE_SYSUTIL_NP_TROPHY,
};

static int sysmodule_id_valid(u16 id)
{
    for (u32 i = 0; i < sizeof(s_valid_ids)/sizeof(s_valid_ids[0]); i++)
        if (s_valid_ids[i] == id)
            return 1;
    return 0;
}

static int sysmodule_id_autoloaded(u16 id)
{
    for (u32 i = 0; i < sizeof(s_autoloaded_ids)/sizeof(s_autoloaded_ids[0]); i++)
        if (s_autoloaded_ids[i] == id)
            return 1;
    return 0;
}

static void sysmodule_ensure_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    for (u32 i = 0; i < sizeof(s_autoloaded_ids)/sizeof(s_autoloaded_ids[0]); i++) {
        u16 id = s_autoloaded_ids[i];
        if (id < CELL_SYSMODULE_MAX_ID)
            s_module_loaded[id] = 1;
        else if ((u16)(id - 0xF000u) < 0x100u)
            s_module_loaded_hi[id - 0xF000u] = 1;
    }
}

static int* sysmodule_loaded_slot(u16 id)
{
    if (!sysmodule_id_valid(id))
        return NULL;
    if (id < CELL_SYSMODULE_MAX_ID)
        return &s_module_loaded[id];
    if ((u16)(id - 0xF000u) < 0x100u)
        return &s_module_loaded_hi[id - 0xF000u];
    return NULL;
}

static const char* sysmodule_id_to_name(u16 id)
{
    switch (id) {
    case CELL_SYSMODULE_NET: return "CELL_SYSMODULE_NET";
    case CELL_SYSMODULE_HTTP: return "CELL_SYSMODULE_HTTP";
    case CELL_SYSMODULE_HTTP_UTIL: return "CELL_SYSMODULE_HTTP_UTIL";
    case CELL_SYSMODULE_SSL: return "CELL_SYSMODULE_SSL";
    case CELL_SYSMODULE_HTTPS: return "CELL_SYSMODULE_HTTPS";
    case CELL_SYSMODULE_VDEC: return "CELL_SYSMODULE_VDEC";
    case CELL_SYSMODULE_ADEC: return "CELL_SYSMODULE_ADEC";
    case CELL_SYSMODULE_DMUX: return "CELL_SYSMODULE_DMUX";
    case CELL_SYSMODULE_VPOST: return "CELL_SYSMODULE_VPOST";
    case CELL_SYSMODULE_RTC: return "CELL_SYSMODULE_RTC";
    case CELL_SYSMODULE_SPURS: return "CELL_SYSMODULE_SPURS";
    case CELL_SYSMODULE_OVIS: return "CELL_SYSMODULE_OVIS";
    case CELL_SYSMODULE_SHEAP: return "CELL_SYSMODULE_SHEAP";
    case CELL_SYSMODULE_SYNC: return "CELL_SYSMODULE_SYNC";
    case CELL_SYSMODULE_FS: return "CELL_SYSMODULE_FS";
    case CELL_SYSMODULE_JPGDEC: return "CELL_SYSMODULE_JPGDEC";
    case CELL_SYSMODULE_GCM_SYS: return "CELL_SYSMODULE_GCM_SYS";
    case CELL_SYSMODULE_AUDIO: return "CELL_SYSMODULE_AUDIO";
    case CELL_SYSMODULE_PAMF: return "CELL_SYSMODULE_PAMF";
    case CELL_SYSMODULE_ATRAC3PLUS: return "CELL_SYSMODULE_ATRAC3PLUS";
    case CELL_SYSMODULE_NETCTL: return "CELL_SYSMODULE_NETCTL";
    case CELL_SYSMODULE_SYSUTIL: return "CELL_SYSMODULE_SYSUTIL";
    case CELL_SYSMODULE_SYSUTIL_NP: return "CELL_SYSMODULE_SYSUTIL_NP";
    case CELL_SYSMODULE_IO: return "CELL_SYSMODULE_IO";
    case CELL_SYSMODULE_PNGDEC: return "CELL_SYSMODULE_PNGDEC";
    case CELL_SYSMODULE_FONT: return "CELL_SYSMODULE_FONT";
    case CELL_SYSMODULE_FONTFT: return "CELL_SYSMODULE_FONTFT";
    case CELL_SYSMODULE_FREETYPE: return "CELL_SYSMODULE_FREETYPE";
    case CELL_SYSMODULE_USBD: return "CELL_SYSMODULE_USBD";
    case CELL_SYSMODULE_SAIL: return "CELL_SYSMODULE_SAIL";
    case CELL_SYSMODULE_L10N: return "CELL_SYSMODULE_L10N";
    case CELL_SYSMODULE_RESC: return "CELL_SYSMODULE_RESC";
    case CELL_SYSMODULE_DAISY: return "CELL_SYSMODULE_DAISY";
    case CELL_SYSMODULE_KEY2CHAR: return "CELL_SYSMODULE_KEY2CHAR";
    case CELL_SYSMODULE_MIC: return "CELL_SYSMODULE_MIC";
    case CELL_SYSMODULE_CAMERA: return "CELL_SYSMODULE_CAMERA";
    case CELL_SYSMODULE_VDEC_MPEG2: return "CELL_SYSMODULE_VDEC_MPEG2";
    case CELL_SYSMODULE_VDEC_AVC: return "CELL_SYSMODULE_VDEC_AVC";
    case CELL_SYSMODULE_ADEC_LPCM: return "CELL_SYSMODULE_ADEC_LPCM";
    case CELL_SYSMODULE_ADEC_AC3: return "CELL_SYSMODULE_ADEC_AC3";
    case CELL_SYSMODULE_ADEC_ATX: return "CELL_SYSMODULE_ADEC_ATX";
    case CELL_SYSMODULE_ADEC_AT3: return "CELL_SYSMODULE_ADEC_AT3";
    case CELL_SYSMODULE_DMUX_PAMF: return "CELL_SYSMODULE_DMUX_PAMF";
    case CELL_SYSMODULE_VDEC_AL: return "CELL_SYSMODULE_VDEC_AL";
    case CELL_SYSMODULE_ADEC_AL: return "CELL_SYSMODULE_ADEC_AL";
    case CELL_SYSMODULE_DMUX_AL: return "CELL_SYSMODULE_DMUX_AL";
    case CELL_SYSMODULE_LV2DBG: return "CELL_SYSMODULE_LV2DBG";
    case CELL_SYSMODULE_USBPSPCM: return "CELL_SYSMODULE_USBPSPCM";
    case CELL_SYSMODULE_AVCONF_EXT: return "CELL_SYSMODULE_AVCONF_EXT";
    case CELL_SYSMODULE_SYSUTIL_USERINFO: return "CELL_SYSMODULE_SYSUTIL_USERINFO";
    case CELL_SYSMODULE_SYSUTIL_SAVEDATA: return "CELL_SYSMODULE_SYSUTIL_SAVEDATA";
    case CELL_SYSMODULE_SUBDISPLAY: return "CELL_SYSMODULE_SUBDISPLAY";
    case CELL_SYSMODULE_SYSUTIL_REC: return "CELL_SYSMODULE_SYSUTIL_REC";
    case CELL_SYSMODULE_VIDEO_EXPORT: return "CELL_SYSMODULE_VIDEO_EXPORT";
    case CELL_SYSMODULE_SYSUTIL_GAME_EXEC: return "CELL_SYSMODULE_SYSUTIL_GAME_EXEC";
    case CELL_SYSMODULE_SYSUTIL_NP2: return "CELL_SYSMODULE_SYSUTIL_NP2";
    case CELL_SYSMODULE_SYSUTIL_AP: return "CELL_SYSMODULE_SYSUTIL_AP";
    case CELL_SYSMODULE_SYSUTIL_NP_CLANS: return "CELL_SYSMODULE_SYSUTIL_NP_CLANS";
    case CELL_SYSMODULE_SYSUTIL_OSK_EXT: return "CELL_SYSMODULE_SYSUTIL_OSK_EXT";
    case CELL_SYSMODULE_VDEC_DIVX: return "CELL_SYSMODULE_VDEC_DIVX";
    case CELL_SYSMODULE_JPGENC: return "CELL_SYSMODULE_JPGENC";
    case CELL_SYSMODULE_SYSUTIL_GAME: return "CELL_SYSMODULE_SYSUTIL_GAME";
    case CELL_SYSMODULE_BGDL: return "CELL_SYSMODULE_BGDL";
    case CELL_SYSMODULE_FREETYPE_TT: return "CELL_SYSMODULE_FREETYPE_TT";
    case CELL_SYSMODULE_SYSUTIL_VIDEO_UPLOAD: return "CELL_SYSMODULE_SYSUTIL_VIDEO_UPLOAD";
    case CELL_SYSMODULE_SYSUTIL_SYSCONF_EXT: return "CELL_SYSMODULE_SYSUTIL_SYSCONF_EXT";
    case CELL_SYSMODULE_FIBER: return "CELL_SYSMODULE_FIBER";
    case CELL_SYSMODULE_SYSUTIL_NP_COMMERCE2: return "CELL_SYSMODULE_SYSUTIL_NP_COMMERCE2";
    case CELL_SYSMODULE_SYSUTIL_NP_TUS: return "CELL_SYSMODULE_SYSUTIL_NP_TUS";
    case CELL_SYSMODULE_VOICE: return "CELL_SYSMODULE_VOICE";
    case CELL_SYSMODULE_ADEC_CELP8: return "CELL_SYSMODULE_ADEC_CELP8";
    case CELL_SYSMODULE_CELP8ENC: return "CELL_SYSMODULE_CELP8ENC";
    case CELL_SYSMODULE_SYSUTIL_LICENSEAREA: return "CELL_SYSMODULE_SYSUTIL_LICENSEAREA";
    case CELL_SYSMODULE_SYSUTIL_MUSIC2: return "CELL_SYSMODULE_SYSUTIL_MUSIC2";
    case CELL_SYSMODULE_SYSUTIL_SCREENSHOT: return "CELL_SYSMODULE_SYSUTIL_SCREENSHOT";
    case CELL_SYSMODULE_SYSUTIL_MUSIC_DECODE: return "CELL_SYSMODULE_SYSUTIL_MUSIC_DECODE";
    case CELL_SYSMODULE_SPURS_JQ: return "CELL_SYSMODULE_SPURS_JQ";
    case CELL_SYSMODULE_PNGENC: return "CELL_SYSMODULE_PNGENC";
    case CELL_SYSMODULE_SYSUTIL_MUSIC_DECODE2: return "CELL_SYSMODULE_SYSUTIL_MUSIC_DECODE2";
    case CELL_SYSMODULE_SYNC2: return "CELL_SYSMODULE_SYNC2";
    case CELL_SYSMODULE_SYSUTIL_NP_UTIL: return "CELL_SYSMODULE_SYSUTIL_NP_UTIL";
    case CELL_SYSMODULE_RUDP: return "CELL_SYSMODULE_RUDP";
    case CELL_SYSMODULE_SYSUTIL_NP_SNS: return "CELL_SYSMODULE_SYSUTIL_NP_SNS";
    case CELL_SYSMODULE_GEM: return "CELL_SYSMODULE_GEM";
    case CELL_SYSMODULE_SYSUTIL_CROSS_CONTROLLER: return "CELL_SYSMODULE_SYSUTIL_CROSS_CONTROLLER";
    case CELL_SYSMODULE_CELPENC: return "CELL_SYSMODULE_CELPENC";
    case CELL_SYSMODULE_GIFDEC: return "CELL_SYSMODULE_GIFDEC";
    case CELL_SYSMODULE_ADEC_CELP: return "CELL_SYSMODULE_ADEC_CELP";
    case CELL_SYSMODULE_ADEC_M2BC: return "CELL_SYSMODULE_ADEC_M2BC";
    case CELL_SYSMODULE_ADEC_M4AAC: return "CELL_SYSMODULE_ADEC_M4AAC";
    case CELL_SYSMODULE_ADEC_MP3: return "CELL_SYSMODULE_ADEC_MP3";
    case CELL_SYSMODULE_IMEJP: return "CELL_SYSMODULE_IMEJP";
    case CELL_SYSMODULE_SYSUTIL_MUSIC: return "CELL_SYSMODULE_SYSUTIL_MUSIC";
    case CELL_SYSMODULE_PHOTO_EXPORT: return "CELL_SYSMODULE_PHOTO_EXPORT";
    case CELL_SYSMODULE_PRINT: return "CELL_SYSMODULE_PRINT";
    case CELL_SYSMODULE_PHOTO_IMPORT: return "CELL_SYSMODULE_PHOTO_IMPORT";
    case CELL_SYSMODULE_MUSIC_EXPORT: return "CELL_SYSMODULE_MUSIC_EXPORT";
    case CELL_SYSMODULE_PHOTO_DECODE: return "CELL_SYSMODULE_PHOTO_DECODE";
    case CELL_SYSMODULE_SYSUTIL_SEARCH: return "CELL_SYSMODULE_SYSUTIL_SEARCH";
    case CELL_SYSMODULE_SYSUTIL_AVCHAT2: return "CELL_SYSMODULE_SYSUTIL_AVCHAT2";
    case CELL_SYSMODULE_SAIL_REC: return "CELL_SYSMODULE_SAIL_REC";
    case CELL_SYSMODULE_SYSUTIL_NP_TROPHY: return "CELL_SYSMODULE_SYSUTIL_NP_TROPHY";
    case CELL_SYSMODULE_LIBATRAC3MULTI: return "CELL_SYSMODULE_LIBATRAC3MULTI";
    default: return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x26A6E12B */
s32 cellSysmoduleLoadModule(u16 id)
{
    int* loaded;

    sysmodule_ensure_init();
    printf("[cellSysmodule] LoadModule(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    /* Already loaded: CELL_OK is the documented contract (Reference p.9 --
     * DUPLICATED is not in LoadModule's return set). */
    *loaded = 1;
    return CELL_OK;
}

/* NID: 0x112A5EE9 */
s32 cellSysmoduleUnloadModule(u16 id)
{
    int* loaded;

    sysmodule_ensure_init();
    printf("[cellSysmodule] UnloadModule(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    /* Auto-loaded modules stay resident (Overview p.12); an already-unloaded
     * module is CELL_OK, not ERROR_UNLOADED (Reference p.10 -- the old error
     * here was the classic "shutdown asserts" landmine). */
    if (!sysmodule_id_autoloaded(id))
        *loaded = 0;
    return CELL_OK;
}

/* NID: 0x5A59E258 */
s32 cellSysmoduleIsLoaded(u16 id)
{
    int* loaded;

    sysmodule_ensure_init();
    printf("[cellSysmodule] IsLoaded(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    return *loaded ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}
