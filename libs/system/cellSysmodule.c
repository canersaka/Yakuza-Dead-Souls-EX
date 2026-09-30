/*
 * ps3recomp - cellSysmodule HLE implementation
 *
 * Tracks loaded state per module id. Modules the firmware auto-loads report
 * loaded from the first call and cannot be unloaded. UnloadModule of a
 * not-loaded module returns CELL_OK; ERROR_UNLOADED is returned by IsLoaded
 * only. Unknown ids return ERROR_UNKNOWN.
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
 * not load them and cannot unload them. */
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

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x26A6E12B */
s32 cellSysmoduleLoadModule(u16 id)
{
    int* loaded;

    sysmodule_ensure_init();

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    /* Already loaded: CELL_OK is the documented contract (DUPLICATED is not
     * in LoadModule's return set). */
    *loaded = 1;
    return CELL_OK;
}

/* NID: 0x112A5EE9 */
s32 cellSysmoduleUnloadModule(u16 id)
{
    int* loaded;

    sysmodule_ensure_init();

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    /* Auto-loaded modules stay resident; an already-unloaded module is
     * CELL_OK, not ERROR_UNLOADED (returning an error here trips shutdown
     * asserts in games). */
    if (!sysmodule_id_autoloaded(id))
        *loaded = 0;
    return CELL_OK;
}

/* NID: 0x5A59E258 */
s32 cellSysmoduleIsLoaded(u16 id)
{
    int* loaded;

    sysmodule_ensure_init();

    loaded = sysmodule_loaded_slot(id);
    if (!loaded)
        return CELL_SYSMODULE_ERROR_UNKNOWN;

    return *loaded ? CELL_OK : CELL_SYSMODULE_ERROR_UNLOADED;
}
