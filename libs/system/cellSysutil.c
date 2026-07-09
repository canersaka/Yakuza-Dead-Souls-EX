/*
 * ps3recomp - cellSysutil HLE implementation
 *
 * System callbacks, parameter queries, BGM control, system cache,
 * and disc game check.
 */

#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Guest callback dispatch hook (set by the game's host code at startup).
 * -----------------------------------------------------------------------*/
ps3_guest_caller_fn g_ps3_guest_caller = NULL;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Stored as a guest OPD address rather than a host function pointer:
     * the recompiled game passes its OPD addr through Register, and the
     * dispatcher hook resolves the OPD when actually invoking. The
     * CellSysutilCallback typedef maps to a pointer, so we cast through
     * uintptr_t at the API boundary. */
    uint32_t            guest_opd;
    uint32_t            userdata;     /* guest pointer; opaque to the runtime */
    int                 registered;
} SysutilCallbackSlot;

static SysutilCallbackSlot s_callbacks[CELL_SYSUTIL_MAX_CALLBACKS];

/* ---------------------------------------------------------------------------
 * Sysutil event queue.
 *
 * The OS would deliver events asynchronously (XMB open/close, drawing
 * begin/end, save-data complete, etc.). Games drain them by calling
 * cellSysutilCheckCallback periodically. We accept queued events from
 * the host side via cellSysutilQueueEvent() and drain them here.
 * -----------------------------------------------------------------------*/

typedef struct {
    int      slot;
    uint32_t status;
    uint32_t param;
} SysutilEvent;

#define SYSUTIL_EVENT_QUEUE_SIZE 32
static SysutilEvent s_event_queue[SYSUTIL_EVENT_QUEUE_SIZE];
static int          s_event_head = 0;  /* next read */
static int          s_event_tail = 0;  /* next write */

void cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param)
{
    int next = (s_event_tail + 1) % SYSUTIL_EVENT_QUEUE_SIZE;
    if (next == s_event_head) {
        printf("[cellSysutil] event queue full — dropping status=0x%X\n", status);
        return;
    }
    s_event_queue[s_event_tail].slot   = slot;
    s_event_queue[s_event_tail].status = status;
    s_event_queue[s_event_tail].param  = param;
    s_event_tail = next;
}

static s32 s_bgm_enabled = 1;
static s32 s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
static char s_cache_path[CELL_SYSCACHE_PATH_MAX];
static int s_cache_mounted = 0;

static void (*s_disc_change_cb)(void*) = NULL;
static void* s_disc_change_arg = NULL;

/* ---------------------------------------------------------------------------
 * Core callbacks & params
 * -----------------------------------------------------------------------*/

s32 cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata)
{
    /* `func` and `userdata` are guest VM addresses; reinterpret without
     * touching them. NULL func means "unregister this slot" — some games
     * (notably flOw) call Register with func=NULL as a clear-on-poll. */
    uint32_t func_addr     = (uint32_t)(uintptr_t)func;
    uint32_t userdata_addr = (uint32_t)(uintptr_t)userdata;

    printf("[cellSysutil] RegisterCallback(slot=%d, func=0x%08X)\n",
           slot, func_addr);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    if (func_addr == 0) {
        /* Treat as Unregister — matches observed game behaviour. */
        s_callbacks[slot].guest_opd  = 0;
        s_callbacks[slot].userdata   = 0;
        s_callbacks[slot].registered = 0;
        return CELL_OK;
    }

    s_callbacks[slot].guest_opd  = func_addr;
    s_callbacks[slot].userdata   = userdata_addr;
    s_callbacks[slot].registered = 1;

    return CELL_OK;
}

s32 cellSysutilUnregisterCallback(s32 slot)
{
    printf("[cellSysutil] UnregisterCallback(slot=%d)\n", slot);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    s_callbacks[slot].guest_opd  = 0;
    s_callbacks[slot].userdata   = 0;
    s_callbacks[slot].registered = 0;

    return CELL_OK;
}

s32 cellSysutilCheckCallback(void)
{
    /* Drain the event queue, dispatching each event into guest code via
     * the registered ps3_guest_caller hook. Standard PS3 sysutil callback
     * signature is:
     *   void cb(uint64_t status, uint64_t param, void* userdata)
     * which maps to PPC r3=status, r4=param, r5=userdata; r6 is unused.
     *
     * Stops when the queue empties OR when no guest caller is installed
     * (in which case we drop pending events to avoid stalling).
     */
    while (s_event_head != s_event_tail) {
        SysutilEvent e = s_event_queue[s_event_head];
        s_event_head = (s_event_head + 1) % SYSUTIL_EVENT_QUEUE_SIZE;

        if (e.slot < 0 || e.slot >= CELL_SYSUTIL_MAX_CALLBACKS) continue;
        if (!s_callbacks[e.slot].registered) continue;
        if (!s_callbacks[e.slot].guest_opd) continue;

        if (g_ps3_guest_caller) {
            g_ps3_guest_caller(s_callbacks[e.slot].guest_opd,
                               (uint64_t)e.status,
                               (uint64_t)e.param,
                               (uint64_t)s_callbacks[e.slot].userdata,
                               0);
        }
    }
    return CELL_OK;
}

s32 cellSysutilGetSystemParamInt(s32 id, s32* value)
{
    if (!value)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_LANG:
        *value = CELL_SYSUTIL_LANG_ENGLISH_US;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN:
        *value = CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT:
        *value = 0; /* YYYYMMDD */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT:
        *value = 0; /* 24-hour */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE:
        *value = 0; /* UTC */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME:
        *value = 0; /* No DST */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL:
        *value = 0; /* No restriction */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT:
        *value = 0;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT:
        *value = 1; /* Has NP account */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ:
        *value = 0; /* 60Hz */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE:
        *value = 1; /* Rumble on */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE:
        *value = 0; /* US/101 */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF:
        *value = 0; /* Disabled */
        break;
    default:
        printf("[cellSysutil] GetSystemParamInt: unknown id 0x%04X\n", id);
        *value = 0;
        break;
    }

    return CELL_OK;
}

s32 cellSysutilGetSystemParamString(s32 id, char* buf, u32 bufsize)
{
    if (!buf || bufsize == 0)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME:
        strncpy(buf, "ps3recomp_user", bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME:
        strncpy(buf, "User", bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    default:
        printf("[cellSysutil] GetSystemParamString: unknown id 0x%04X\n", id);
        buf[0] = '\0';
        break;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * BGM playback control
 * -----------------------------------------------------------------------*/

s32 cellSysutilEnableBgmPlayback(void)
{
    printf("[cellSysutil] EnableBgmPlayback()\n");
    s_bgm_enabled = 1;
    return CELL_OK;
}

s32 cellSysutilDisableBgmPlayback(void)
{
    printf("[cellSysutil] DisableBgmPlayback()\n");
    s_bgm_enabled = 0;
    s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
    return CELL_OK;
}

s32 cellSysutilGetBgmPlaybackStatus(s32* status)
{
    if (!status)
        return CELL_SYSUTIL_ERROR_VALUE;

    *status = s_bgm_status;
    return CELL_OK;
}

s32 cellSysutilSetBgmPlaybackExtraParam(void* param)
{
    (void)param;
    printf("[cellSysutil] SetBgmPlaybackExtraParam()\n");
    return CELL_OK;
}

s32 cellSysutilEnableBgmPlaybackEx(s32 param)
{
    (void)param;
    printf("[cellSysutil] EnableBgmPlaybackEx(%d)\n", param);
    s_bgm_enabled = 1;
    return CELL_OK;
}

s32 cellSysutilDisableBgmPlaybackEx(void)
{
    printf("[cellSysutil] DisableBgmPlaybackEx()\n");
    s_bgm_enabled = 0;
    s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * System cache
 * -----------------------------------------------------------------------*/

s32 cellSysCacheMount(char* param)
{
    /* s23 conformance fix: the argument is a CellSysCacheParam STRUCT
     * (RPCS3 cellSysutil.h: char cacheId[32]; char getCachePath[1055+1];
     * void* reserved), not a flat string buffer. The old code overwrote the
     * game's cacheId INPUT field (offset 0) with our mount path and never
     * wrote getCachePath (offset 32) -- guest memory corruption on a path
     * reached every boot (s23boot1.err:7821). Read cacheId, write the
     * mount path into getCachePath, leave cacheId untouched. */
    char* cache_id      = param;            /* in: caller's cache name  */
    char* get_cache_path = param + 32;      /* out: mounted path, 1055B */

    if (!param)
        return CELL_EINVAL;

    printf("[cellSysutil] SysCacheMount(cacheId='%.32s')\n", cache_id);

    strncpy(s_cache_path, "/dev_hdd1/caches", CELL_SYSCACHE_PATH_MAX - 1);
    s_cache_path[CELL_SYSCACHE_PATH_MAX - 1] = '\0';
    strncpy(get_cache_path, s_cache_path, CELL_SYSCACHE_PATH_MAX - 1);
    get_cache_path[CELL_SYSCACHE_PATH_MAX - 1] = '\0';
    s_cache_mounted = 1;

    return CELL_OK;
}

s32 cellSysCacheClear(void)
{
    printf("[cellSysutil] SysCacheClear()\n");
    s_cache_mounted = 0;
    s_cache_path[0] = '\0';
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Disc game check
 * -----------------------------------------------------------------------*/

s32 cellDiscGameGetBootDiscInfo(u32* type, char* titleId, u32 titleIdSize)
{
    printf("[cellSysutil] DiscGameGetBootDiscInfo()\n");

    if (type)
        *type = CELL_DISCGAME_TYPE_HDD; /* pretend HDD game */

    if (titleId && titleIdSize > 0) {
        /* Use the real title id (from PARAM.SFO at boot) instead of a placeholder. */
        extern const char* cellGame_get_title_id(void);
        const char* tid = cellGame_get_title_id();
        strncpy(titleId, tid && tid[0] ? tid : "GAME00000", titleIdSize - 1);
        titleId[titleIdSize - 1] = '\0';
    }

    return CELL_OK;
}

s32 cellDiscGameRegisterDiscChangeCallback(void (*callback)(void*), void* arg)
{
    printf("[cellSysutil] DiscGameRegisterDiscChangeCallback()\n");
    s_disc_change_cb = callback;
    s_disc_change_arg = arg;
    return CELL_OK;
}

s32 cellDiscGameUnregisterDiscChangeCallback(void)
{
    printf("[cellSysutil] DiscGameUnregisterDiscChangeCallback()\n");
    s_disc_change_cb = NULL;
    s_disc_change_arg = NULL;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc utilities
 * -----------------------------------------------------------------------*/

s32 cellSysutilGetLicenseArea(void)
{
    /* Return 'A' for America */
    return 'A';
}

s32 cellSysutilIsMeetingApp(void)
{
    return 0; /* Not a meeting app */
}
