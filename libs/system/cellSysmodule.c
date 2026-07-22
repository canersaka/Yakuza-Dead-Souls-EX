/*
 * ps3recomp - cellSysmodule HLE stub implementation
 */

#include "cellSysmodule.h"
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Track which modules have been "loaded" */
static int s_module_loaded[CELL_SYSMODULE_MAX_ID];
static int s_np_trophy_module_loaded;

static int* sysmodule_loaded_slot(u16 id)
{
    if (id < CELL_SYSMODULE_MAX_ID)
        return &s_module_loaded[id];

    switch (id) {
    case CELL_SYSMODULE_SYSUTIL_NP_TROPHY:
        return &s_np_trophy_module_loaded;
    default:
        return NULL;
    }
}

static const char* sysmodule_id_to_name(u16 id)
{
    switch (id) {
    case CELL_SYSMODULE_NET:           return "CELL_SYSMODULE_NET";
    case CELL_SYSMODULE_HTTP:          return "CELL_SYSMODULE_HTTP";
    case CELL_SYSMODULE_HTTP_UTIL:     return "CELL_SYSMODULE_HTTP_UTIL";
    case CELL_SYSMODULE_SSL:           return "CELL_SYSMODULE_SSL";
    case CELL_SYSMODULE_HTTPS:         return "CELL_SYSMODULE_HTTPS";
    case CELL_SYSMODULE_VDEC:          return "CELL_SYSMODULE_VDEC";
    case CELL_SYSMODULE_ADEC:          return "CELL_SYSMODULE_ADEC";
    case CELL_SYSMODULE_DMUX:          return "CELL_SYSMODULE_DMUX";
    case CELL_SYSMODULE_VPOST:         return "CELL_SYSMODULE_VPOST";
    case CELL_SYSMODULE_RTC:           return "CELL_SYSMODULE_RTC";
    case CELL_SYSMODULE_SPURS:         return "CELL_SYSMODULE_SPURS";
    case CELL_SYSMODULE_OVIS:          return "CELL_SYSMODULE_OVIS";
    case CELL_SYSMODULE_SHEAP:         return "CELL_SYSMODULE_SHEAP";
    case CELL_SYSMODULE_SYNC:          return "CELL_SYSMODULE_SYNC";
    case CELL_SYSMODULE_SYNC2:         return "CELL_SYSMODULE_SYNC2";
    case CELL_SYSMODULE_FS:            return "CELL_SYSMODULE_FS";
    case CELL_SYSMODULE_JPGDEC:        return "CELL_SYSMODULE_JPGDEC";
    case CELL_SYSMODULE_GCM_SYS:       return "CELL_SYSMODULE_GCM_SYS";
    case CELL_SYSMODULE_AUDIO:         return "CELL_SYSMODULE_AUDIO";
    case CELL_SYSMODULE_PAMF:          return "CELL_SYSMODULE_PAMF";
    case CELL_SYSMODULE_ATRAC3PLUS:    return "CELL_SYSMODULE_ATRAC3PLUS";
    case CELL_SYSMODULE_NETCTL:        return "CELL_SYSMODULE_NETCTL";
    case CELL_SYSMODULE_SYSUTIL:       return "CELL_SYSMODULE_SYSUTIL";
    case CELL_SYSMODULE_SYSUTIL_NP:    return "CELL_SYSMODULE_SYSUTIL_NP";
    case CELL_SYSMODULE_IO:            return "CELL_SYSMODULE_IO";
    case CELL_SYSMODULE_PNGDEC:        return "CELL_SYSMODULE_PNGDEC";
    case CELL_SYSMODULE_FONT:          return "CELL_SYSMODULE_FONT";
    case CELL_SYSMODULE_FREETYPE:      return "CELL_SYSMODULE_FREETYPE";
    case CELL_SYSMODULE_USBD:          return "CELL_SYSMODULE_USBD";
    case CELL_SYSMODULE_SAIL:          return "CELL_SYSMODULE_SAIL";
    case CELL_SYSMODULE_L10N:          return "CELL_SYSMODULE_L10N";
    case CELL_SYSMODULE_RESC:          return "CELL_SYSMODULE_RESC";
    case CELL_SYSMODULE_SYSUTIL_SAVEDATA: return "CELL_SYSMODULE_SYSUTIL_SAVEDATA";
    case CELL_SYSMODULE_SYSUTIL_GAME:  return "CELL_SYSMODULE_SYSUTIL_GAME";
    case CELL_SYSMODULE_FIBER:         return "CELL_SYSMODULE_FIBER";
    case CELL_SYSMODULE_GEM:           return "CELL_SYSMODULE_GEM";
    case CELL_SYSMODULE_SYSUTIL_NP_TROPHY: return "CELL_SYSMODULE_SYSUTIL_NP_TROPHY";
    default:                           return "UNKNOWN";
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x26A6E12B */
s32 cellSysmoduleLoadModule(u16 id)
{
    int* loaded;

    printf("[cellSysmodule] LoadModule(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    if (*loaded)
        return CELL_OK;  /* Already loaded — return success (some games treat DUPLICATED as fatal) */

    *loaded = 1;
    return CELL_OK;
}

/* NID: 0x112A5EE9 */
s32 cellSysmoduleUnloadModule(u16 id)
{
    int* loaded;

    printf("[cellSysmodule] UnloadModule(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    if (!*loaded)
        return CELL_SYSMODULE_ERROR_UNLOADED;

    *loaded = 0;
    return CELL_OK;
}

/* NID: 0x5A59E258 */
s32 cellSysmoduleIsLoaded(u16 id)
{
    int* loaded;

    printf("[cellSysmodule] IsLoaded(id=0x%04X '%s')\n",
           id, sysmodule_id_to_name(id));

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    return *loaded ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}
