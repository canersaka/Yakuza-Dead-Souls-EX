/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file at gamedata/trophies/{commId}.json.
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 *
 * Timestamps (2026-08-05 flow fixes): T is a CellRtcTick -- microseconds
 * since 0001-01-01 00:00:00. ORACLE(librtc-Reference_e.pdf p.5 "CellRtcTick"
 * + pp.19-20 cellRtcGetTick/SetTick: "cumulative time in terms of 1
 * microsecond units starting from 0001/01/01 00:00:00"). Files written by
 * older builds stored Unix SECONDS; those are migrated on load (any nonzero
 * value < 1e12 is treated as seconds -- Unix seconds stay < ~2e9 for
 * centuries while any tick for a date >= 1970 is >= 6.2e16) and rewritten
 * as ticks on the next save.
 *
 * Trophy configuration (2026-08-05): real trophy count, grades, hidden
 * flags and the platinum trophy id are parsed from the game's installed
 * trophy pack (TROPDIR/{commId}_{num}/TROPHY.TRP -> TROPCONF.SFM). When no
 * pack is present the legacy 128-trophy default remains in effect.
 */

#include "sceNpTrophy.h"
#include "ps3emu/guest_call.h"
#include "../filesystem/cellFs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_trophy_initialized = 0;
static char s_storage_path[512] = "gamedata/trophies";

static void trophy_store_be32(void* dst, u32 value)
{
    u8* p = (u8*)dst;
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static void trophy_store_be64(void* dst, u64 value)
{
    u8* p = (u8*)dst;
    p[0] = (u8)(value >> 56);
    p[1] = (u8)(value >> 48);
    p[2] = (u8)(value >> 40);
    p[3] = (u8)(value >> 32);
    p[4] = (u8)(value >> 24);
    p[5] = (u8)(value >> 16);
    p[6] = (u8)(value >> 8);
    p[7] = (u8)value;
}

/* Per-context data */
typedef struct {
    int                      in_use;
    int                      registered;
    int                      read_only;     /* SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY */
    int                      state_loaded;  /* config + saved unlocks loaded */
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    /* CellRtcTick microseconds since 0001-01-01; 0 = locked. */
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
    /* Trophy configuration from TROPCONF.SFM (have_config != 0). */
    int                      have_config;
    s32                      platinum_id;   /* -1 if the set has no platinum */
    u8                       grade[SCE_NP_TROPHY_MAX_NUM_TROPHIES];   /* SCE_NP_TROPHY_GRADE_* */
    u8                       hidden[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    s16                      pid[SCE_NP_TROPHY_MAX_NUM_TROPHIES];     /* platinum-group id; -1 = unspecified */
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

/* Public IDs are positive; slot zero is reserved as the invalid sentinel. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

/* ---------------------------------------------------------------------------
 * Persistent storage helpers
 * -----------------------------------------------------------------------*/

static void trophy_ensure_dir(void)
{
    char tmp[512];
    char* p;

    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const SceNpCommunicationId* commId,
                                 char* buf, size_t bufsize)
{
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, commId->data);
}

/* ---------------------------------------------------------------------------
 * Timestamp conversion: persisted + API timestamps are CellRtcTick values.
 *
 * ORACLE(librtc-Reference_e.pdf p.5 "CellRtcTick": "Cumulative number of
 * Ticks from 0001/01/01 00:00:00"; pp.19-20 cellRtcGetTick/cellRtcSetTick:
 * "cumulative time in terms of 1 microsecond units starting from
 * 0001/01/01 00:00:00"). Seconds between 0001-01-01 and 1970-01-01:
 * 62135596800 (the same constant libs/misc's cellRtc uses; verified by
 * computation in the 2026-08-04 cellRtc audit).
 * -----------------------------------------------------------------------*/

#define TROPHY_RTC_EPOCH_DIFF_SEC 62135596800ULL
#define TROPHY_TICKS_PER_SEC      1000000ULL

/* Migration threshold: any nonzero persisted value below this is a legacy
 * Unix-seconds stamp (seconds stay < ~2e9 for centuries; a tick for any
 * date >= 1970 is >= 6.2e16). Values in between cannot legitimately arise
 * from either scheme. */
#define TROPHY_LEGACY_SECONDS_MAX 1000000000000ULL

static u64 trophy_unix_sec_to_tick(u64 unix_sec)
{
    return (unix_sec + TROPHY_RTC_EPOCH_DIFF_SEC) * TROPHY_TICKS_PER_SEC;
}

static u64 trophy_now_tick(void)
{
    return trophy_unix_sec_to_tick((u64)time(NULL));
}

static u64 trophy_normalize_timestamp(u64 persisted)
{
    if (persisted != 0 && persisted < TROPHY_LEGACY_SECONDS_MAX)
        return trophy_unix_sec_to_tick(persisted);   /* legacy seconds */
    return persisted;                                /* already a tick (or 0) */
}

/* ---------------------------------------------------------------------------
 * Trophy configuration (TROPHY.TRP -> TROPCONF.SFM)
 *
 * TROPHY.TRP is a flat big-endian container. Layout verified by direct
 * inspection of the title's own pack, MEASURED(gamedata/dev_bdvd/PS3_GAME/
 * TROPDIR/NPWR02712_00/TROPHY.TRP): magic 0xDCA24D00 at +0x00, version at
 * +0x04, file size (be64) at +0x08, entry count (be32) at +0x10, entry size
 * (be32, 0x40) at +0x14; entry table starts at +0x40 with, per entry,
 * name[32] then be64 offset then be64 size. TROPCONF.SFM inside it is XML:
 *   <trophy id="000" hidden="yes" ttype="P" pid="-1"/>
 * ttype: P/G/S/B grade; pid: platinum-group id ("-1" on the platinum
 * itself). This title's set: 49 trophies, 1P/3G/7S/38B, platinum id 0.
 * -----------------------------------------------------------------------*/

#define TROPHY_TRP_MAGIC     0xDCA24D00u
#define TROPHY_CONF_MAX_SIZE (1024 * 1024)

static u32 trophy_read_be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static u64 trophy_read_be64(const u8* p)
{
    return ((u64)trophy_read_be32(p) << 32) | (u64)trophy_read_be32(p + 4);
}

/* Extract an attribute value (up to valsize-1 chars) from the XML tag text
 * starting at `tag` and ending at the next '>'. Returns 1 on success. */
static int trophy_xml_attr(const char* tag, const char* name,
                           char* val, size_t valsize)
{
    char pattern[32];
    const char* p;
    const char* end = strchr(tag, '>');
    size_t n = 0;

    snprintf(pattern, sizeof(pattern), "%s=\"", name);
    p = strstr(tag, pattern);
    if (!p || (end && p > end))
        return 0;
    p += strlen(pattern);
    while (p[n] && p[n] != '"' && n + 1 < valsize) {
        val[n] = p[n];
        n++;
    }
    val[n] = '\0';
    return 1;
}

/* Read TROPCONF.SFM out of the game's TROPHY.TRP. Returns a malloc'd
 * NUL-terminated buffer (caller frees) or NULL. */
static char* trophy_read_tropconf(const SceNpCommunicationId* commId)
{
    char ps3_path[256];
    char trp_path[1024];
    u8 hdr[0x40];
    FILE* fp;
    u32 entry_count, entry_size;
    char* conf = NULL;

    /* Same {data}_{num:02} directory convention the XMB uses; resolve the
     * guest path through cellFs so a relocated content root keeps working,
     * with the literal default-mapping path as fallback (e.g. unit tests
     * that never initialise cellFs mappings). */
    snprintf(ps3_path, sizeof(ps3_path),
             "/dev_bdvd/PS3_GAME/TROPDIR/%s_%02u/TROPHY.TRP",
             commId->data, (unsigned)commId->num);
    if (cellfs_translate_path(ps3_path, trp_path, sizeof(trp_path)) != 0)
        snprintf(trp_path, sizeof(trp_path),
                 "gamedata/dev_bdvd/PS3_GAME/TROPDIR/%s_%02u/TROPHY.TRP",
                 commId->data, (unsigned)commId->num);

    fp = fopen(trp_path, "rb");
    if (!fp) {
        printf("[sceNpTrophy] no trophy pack at %s (keeping default trophy count)\n",
               trp_path);
        return NULL;
    }

    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        trophy_read_be32(hdr + 0x00) != TROPHY_TRP_MAGIC) {
        printf("[sceNpTrophy] %s: bad TRP header\n", trp_path);
        fclose(fp);
        return NULL;
    }

    entry_count = trophy_read_be32(hdr + 0x10);
    entry_size  = trophy_read_be32(hdr + 0x14);
    if (entry_size < 0x30 || entry_size > 0x100 || entry_count > 4096) {
        printf("[sceNpTrophy] %s: implausible TRP entry table (%u x 0x%X)\n",
               trp_path, entry_count, entry_size);
        fclose(fp);
        return NULL;
    }

    for (u32 i = 0; i < entry_count && !conf; i++) {
        u8 entry[0x100];
        if (fseek(fp, 0x40 + (long)i * (long)entry_size, SEEK_SET) != 0 ||
            fread(entry, 1, entry_size, fp) != entry_size)
            break;
        entry[31] = '\0';
        if (strcmp((const char*)entry, "TROPCONF.SFM") != 0)
            continue;

        u64 off  = trophy_read_be64(entry + 0x20);
        u64 size = trophy_read_be64(entry + 0x28);
        if (size == 0 || size > TROPHY_CONF_MAX_SIZE)
            break;
        conf = (char*)malloc((size_t)size + 1);
        if (!conf)
            break;
        if (fseek(fp, (long)off, SEEK_SET) != 0 ||
            fread(conf, 1, (size_t)size, fp) != (size_t)size) {
            free(conf);
            conf = NULL;
            break;
        }
        conf[size] = '\0';
    }

    fclose(fp);
    if (!conf)
        printf("[sceNpTrophy] %s: TROPCONF.SFM not found/readable\n", trp_path);
    return conf;
}

/* Parse TROPCONF.SFM into the context. Returns 1 when a config was loaded. */
static int trophy_load_config(TrophyContext* ctx)
{
    char* conf;
    const char* p;
    u32 max_id_plus1 = 0;

    if (ctx->have_config)
        return 1;

    conf = trophy_read_tropconf(&ctx->commId);
    if (!conf)
        return 0;

    for (u32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES; i++) {
        ctx->grade[i]  = SCE_NP_TROPHY_GRADE_UNKNOWN;
        ctx->hidden[i] = 0;
        ctx->pid[i]    = -1;
    }
    ctx->platinum_id = -1;

    for (p = strstr(conf, "<trophy "); p; p = strstr(p + 1, "<trophy ")) {
        char val[16];
        u32 id;

        if (!trophy_xml_attr(p, "id", val, sizeof(val)))
            continue;
        id = (u32)strtoul(val, NULL, 10);
        if (id >= SCE_NP_TROPHY_MAX_NUM_TROPHIES)
            continue;
        if (id + 1 > max_id_plus1)
            max_id_plus1 = id + 1;

        if (trophy_xml_attr(p, "ttype", val, sizeof(val))) {
            switch (val[0]) {
            case 'P': ctx->grade[id] = SCE_NP_TROPHY_GRADE_PLATINUM;
                      ctx->platinum_id = (s32)id;                    break;
            case 'G': ctx->grade[id] = SCE_NP_TROPHY_GRADE_GOLD;     break;
            case 'S': ctx->grade[id] = SCE_NP_TROPHY_GRADE_SILVER;   break;
            case 'B': ctx->grade[id] = SCE_NP_TROPHY_GRADE_BRONZE;   break;
            default:  ctx->grade[id] = SCE_NP_TROPHY_GRADE_UNKNOWN;  break;
            }
        }
        if (trophy_xml_attr(p, "hidden", val, sizeof(val)))
            ctx->hidden[id] = (val[0] == 'y') ? 1 : 0;
        if (trophy_xml_attr(p, "pid", val, sizeof(val)))
            ctx->pid[id] = (s16)strtol(val, NULL, 10);
    }

    free(conf);

    if (max_id_plus1 == 0) {
        printf("[sceNpTrophy] TROPCONF.SFM parsed but no <trophy> entries; "
               "keeping default count %u\n", ctx->total_trophies);
        return 0;
    }

    ctx->total_trophies = max_id_plus1;
    ctx->have_config = 1;
    printf("[sceNpTrophy] trophy config: %u trophies, platinum id %d\n",
           ctx->total_trophies, ctx->platinum_id);
    return 1;
}

static void trophy_load(TrophyContext* ctx);   /* defined below */

/* Load config + persisted unlock state once per context. Used by both
 * RegisterContext and the READ_ONLY UnlockState path. */
static void trophy_load_state(TrophyContext* ctx)
{
    if (ctx->state_loaded)
        return;
    trophy_load_config(ctx);
    trophy_load(ctx);
    ctx->state_loaded = 1;
}

/* True when a persisted unlock file exists (our HLE's stand-in for "the
 * trophy set is installed" in the RegisterContext status protocol). */
static int trophy_save_exists(const TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp = NULL;

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));
    fp = fopen(filepath, "r");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

static void trophy_save(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    int first = 1;

    trophy_ensure_dir();
    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "w");
    if (!fp) {
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n",
               filepath);
        return;
    }

    fprintf(fp, "{\n  \"trophies\": [");
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if (ctx->unlocked[i]) {
            if (!first) fprintf(fp, ",");
            fprintf(fp, "\n    { \"id\": %u, \"timestamp\": %llu }",
                    i, (unsigned long long)ctx->unlock_time[i]);
            first = 0;
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
}

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    char line[256];

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "r");
    if (!fp)
        return; /* no saved data, all locked */

    /*
     * Simple JSON parser -- look for "id": N and "timestamp": T patterns.
     * This avoids any external JSON library dependency.
     */
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        if (id_pos) {
            u32 tid = 0;
            u64 ts = 0;
            const char* ts_pos;

            if (sscanf(id_pos, "\"id\": %u", &tid) == 1 &&
                tid < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
                ts_pos = strstr(line, "\"timestamp\":");
                if (ts_pos)
                    sscanf(ts_pos, "\"timestamp\": %llu",
                           (unsigned long long*)&ts);
                ctx->unlocked[tid] = 1;
                /* Legacy files stored Unix seconds; new files store
                 * CellRtcTick microseconds (see file header). */
                ctx->unlock_time[tid] = trophy_normalize_timestamp(ts);
            }
        }
    }
    fclose(fp);

    printf("[sceNpTrophy] Loaded trophy data from %s\n", filepath);
}

static u32 trophy_count_unlocked(TrophyContext* ctx)
{
    u32 count = 0;
    for (u32 i = 0; i < ctx->total_trophies; i++)
        if (ctx->unlocked[i]) count++;
    return count;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Save all registered contexts */
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use && s_contexts[i].registered)
            trophy_save(&s_contexts[i]);
    }

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)commSign;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* NP_Trophy-Reference p.35: the only defined option bit is
     * CREATE_CONTEXT_READ_ONLY; undefined bits -> NOT_SUPPORTED. */
    if (options & ~SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY)
        return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            s_contexts[i].platinum_id = -1;
            s_contexts[i].read_only =
                (options & SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) ? 1 : 0;
            trophy_store_be32(context, (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\"%s) -> ctx=%d\n",
                   commId->data, s_contexts[i].read_only ? ", READ_ONLY" : "", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (s_contexts[context].registered)
        trophy_save(&s_contexts[context]);

    s_contexts[context].in_use = 0;
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            trophy_store_be32(handle, (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    s_handles[handle].in_use = 0;
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

/* Dispatch one status callback into guest code and return its (s32) result.
 *
 * Guest signature (ORACLE np/trophy.h:123-129):
 *   int cb(SceNpTrophyContext context, SceNpTrophyStatus status,
 *          int completed, int total, void *arg)   -> r3..r7, result in r3.
 *
 * `statusCb` is the raw guest OPD address and `arg` the raw guest pointer
 * (the import bridge passes both untranslated -- import_bridges_gen.cpp
 * yz_imp_sceNpTrophyRegisterContext).
 *
 * Delivery deviation, recorded: NP_Trophy-Reference p.23 routes these
 * callbacks through the cellSysutilCheckCallback pump (RegisterContext
 * blocks on a subthread while the main thread pumps). Our sysutil queue
 * only carries the 3-arg sysutil signature, and every module-specific
 * callback in this tree (cellMsgDialog, cellSaveData, cellGcm handlers)
 * dispatches synchronously through the guest-caller hook instead. We follow
 * that convention: in-order synchronous calls on the caller thread. Since
 * RegisterContext is documented blocking (Reference p.44 Notes), the guest
 * still observes every callback complete before the function returns; only
 * the thread affinity of the callback body differs.
 *
 * Returns 0 ("continue") when no guest caller is installed (unit tests). */
static s32 trophy_dispatch_status_cb(SceNpTrophyStatusCallback statusCb,
                                     void* arg, s32 context, u32 status,
                                     s32 completed, s32 total)
{
    uint32_t status_opd = (uint32_t)(uintptr_t)statusCb;
    uint32_t arg_ea     = (uint32_t)(uintptr_t)arg;

    if (!status_opd)
        return 0;

    if (g_ps3_guest_caller_ret) {
        uint64_t r = g_ps3_guest_caller_ret(status_opd,
                                            (uint64_t)(uint32_t)context,
                                            (uint64_t)status,
                                            (uint64_t)(uint32_t)completed,
                                            (uint64_t)(uint32_t)total,
                                            (uint64_t)arg_ea,
                                            0, 0, 0);
        return (s32)(u32)r;
    }
    if (g_ps3_guest_caller) {
        /* Legacy void hook: deliver the notification, assume "continue". */
        g_ps3_guest_caller(status_opd,
                           (uint64_t)(uint32_t)context,
                           (uint64_t)status,
                           (uint64_t)(uint32_t)completed,
                           (uint64_t)(uint32_t)total,
                           (uint64_t)arg_ea,
                           0, 0, 0);
    }
    return 0;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               SceNpTrophyStatusCallback statusCb,
                               void* callbackArg,
                               u64 options)
{
    (void)options;   /* REGISTER_CONTEXT_SHOW_ERROR_EXIT: no dialog UI here */

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    /* NP_Trophy-Reference p.42: NULL statusCb -> INVALID_ARGUMENT. */
    if (!statusCb)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_REG;

    TrophyContext* ctx = &s_contexts[context];

    /* "Installed" in this HLE = a persisted unlock file exists. First boot
     * therefore reports NOT_INSTALLED (the set is installed during this
     * registration); later boots report INSTALLED. */
    const u32 install_status = trophy_save_exists(ctx)
                                   ? SCE_NP_TROPHY_STATUS_INSTALLED
                                   : SCE_NP_TROPHY_STATUS_NOT_INSTALLED;

    trophy_load_state(ctx);

    /* Documented status sequence (NP_Trophy-Reference pp.22-23): the first
     * callback reports the install status; SETUP, PROGRESS xN and FINALIZE
     * follow; COMPLETE terminates. completed/total feed the game's progress
     * bar. A negative return from ANY call -- including COMPLETE ("normal
     * termination when a non-negative number is returned in response to a
     * processing completion notification") -- aborts registration and the
     * function returns PROCESSING_ABORTED. */
    const s32 total = (s32)ctx->total_trophies;
    struct { u32 status; s32 completed; } seq[8];
    int nseq = 0;

    seq[nseq].status = install_status;                       seq[nseq++].completed = 0;
    seq[nseq].status = SCE_NP_TROPHY_STATUS_PROCESSING_SETUP;    seq[nseq++].completed = 0;
    seq[nseq].status = SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS; seq[nseq++].completed = 0;
    seq[nseq].status = SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS; seq[nseq++].completed = total / 2;
    seq[nseq].status = SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS; seq[nseq++].completed = total;
    seq[nseq].status = SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE; seq[nseq++].completed = total;
    seq[nseq].status = SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE; seq[nseq++].completed = total;

    for (int i = 0; i < nseq; i++) {
        s32 cb_ret = trophy_dispatch_status_cb(statusCb, callbackArg,
                                               context, seq[i].status,
                                               seq[i].completed, total);
        if (cb_ret < 0) {
            printf("[sceNpTrophy] RegisterContext(ctx=%d): callback returned %d "
                   "at status %u -> PROCESSING_ABORTED\n",
                   context, cb_ret, seq[i].status);
            return SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED;
        }
    }

    ctx->registered = 1;

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d) -> registered "
           "(%u trophies%s)\n",
           context, handle, ctx->total_trophies,
           ctx->have_config ? "" : ", default count -- no trophy pack");
    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)handle; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* Typical trophy pack size */
    trophy_store_be64(reqSpace, 1024 * 1024); /* 1 MB */
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> 1MB\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    TrophyContext* ctx = &s_contexts[context];

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyGameDetails));
        if (ctx->have_config) {
            u32 n[5] = {0};   /* indexed by SCE_NP_TROPHY_GRADE_* */
            for (u32 i = 0; i < ctx->total_trophies; i++)
                if (ctx->grade[i] <= SCE_NP_TROPHY_GRADE_BRONZE)
                    n[ctx->grade[i]]++;
            trophy_store_be32(&details->numTrophies, ctx->total_trophies);
            trophy_store_be32(&details->numPlatinum, n[SCE_NP_TROPHY_GRADE_PLATINUM]);
            trophy_store_be32(&details->numGold,     n[SCE_NP_TROPHY_GRADE_GOLD]);
            trophy_store_be32(&details->numSilver,   n[SCE_NP_TROPHY_GRADE_SILVER]);
            trophy_store_be32(&details->numBronze,   n[SCE_NP_TROPHY_GRADE_BRONZE]);
        } else {
            /* No trophy pack: keep the legacy placeholder set. */
            trophy_store_be32(&details->numTrophies, 32);
            trophy_store_be32(&details->numPlatinum, 1);
            trophy_store_be32(&details->numGold, 2);
            trophy_store_be32(&details->numSilver, 8);
            trophy_store_be32(&details->numBronze, 21);
        }
        /* Real title/description live in TROP.SFM; not parsed (this entry
         * point is not imported by the current title). */
        strncpy(details->title, "PS3 Game",
                SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE - 1);
        strncpy(details->description, "Trophy set",
                SCE_NP_TROPHY_GAME_DESC_MAX_SIZE - 1);
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyGameData));
        const u32 unlocked = trophy_count_unlocked(ctx);
        u32 u[5] = {0};
        if (ctx->have_config) {
            for (u32 i = 0; i < ctx->total_trophies; i++)
                if (ctx->unlocked[i] && ctx->grade[i] <= SCE_NP_TROPHY_GRADE_BRONZE)
                    u[ctx->grade[i]]++;
        } else {
            u[SCE_NP_TROPHY_GRADE_BRONZE] = unlocked;   /* legacy estimate */
        }
        trophy_store_be32(&data->unlockedTrophies, unlocked);
        trophy_store_be32(&data->unlockedPlatinum, u[SCE_NP_TROPHY_GRADE_PLATINUM]);
        trophy_store_be32(&data->unlockedGold,     u[SCE_NP_TROPHY_GRADE_GOLD]);
        trophy_store_be32(&data->unlockedSilver,   u[SCE_NP_TROPHY_GRADE_SILVER]);
        trophy_store_be32(&data->unlockedBronze,   u[SCE_NP_TROPHY_GRADE_BRONZE]);
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    TrophyContext* ctx = &s_contexts[context];

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyDetails));
        trophy_store_be32(&details->trophyId, (u32)trophyId);
        trophy_store_be32(&details->trophyGrade,
                          ctx->have_config ? (u32)ctx->grade[trophyId]
                                           : SCE_NP_TROPHY_GRADE_BRONZE);
        /* Real name/description live in TROP.SFM; not parsed (this entry
         * point is not imported by the current title). */
        snprintf(details->name, SCE_NP_TROPHY_NAME_MAX_SIZE,
                 "Trophy %d", trophyId);
        snprintf(details->description, SCE_NP_TROPHY_DESC_MAX_SIZE,
                 "Trophy #%d description", trophyId);
        details->hidden = ctx->have_config ? ctx->hidden[trophyId] : 0;
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyData));
        trophy_store_be32(&data->trophyId, (u32)trophyId);
        data->unlocked = ctx->unlocked[trophyId];
        /* CellRtcTick microseconds since 0001-01-01 (see file header). */
        trophy_store_be64(&data->timestamp, ctx->unlock_time[trophyId]);
    }

    return CELL_OK;
}

/* True when every trophy gating the platinum is unlocked. A trophy gates
 * the platinum when it is a member of the platinum group: its pid matches
 * the platinum id, or -- when the config carried no pid attribute -- it is
 * simply any non-platinum trophy. */
static int trophy_platinum_requirements_met(const TrophyContext* ctx)
{
    if (!ctx->have_config || ctx->platinum_id < 0)
        return 0;
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if ((s32)i == ctx->platinum_id)
            continue;
        int gates = (ctx->pid[i] == (s16)ctx->platinum_id) ||
                    (ctx->pid[i] == -1 &&
                     ctx->grade[i] != SCE_NP_TROPHY_GRADE_PLATINUM);
        if (gates && !ctx->unlocked[i])
            return 0;
    }
    return 1;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    TrophyContext* ctx = &s_contexts[context];

    /* A READ_ONLY context exists "to reference trophies without unlocking
     * them" (NP_Trophy-Reference p.35). The exact reject code for trying is
     * not documented; NOT_SUPPORTED is the closest defined one (INFERRED). */
    if (ctx->read_only)
        return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;

    /* Out-of-range id: "The trophy ID may exceed the maximum allowable
     * number of trophies" -> INVALID_TROPHY_ID (Reference p.48). */
    if (trophyId < 0 || (u32)trophyId >= ctx->total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;

    /* "The platinum trophy is unlocked by the system and cannot be unlocked
     * by this function" (Reference p.48). */
    if (ctx->have_config && ctx->platinum_id >= 0 &&
        trophyId == ctx->platinum_id)
        return SCE_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK;

    if (ctx->unlocked[trophyId])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    /* Unlock the trophy */
    ctx->unlocked[trophyId] = 1;
    ctx->unlock_time[trophyId] = trophy_now_tick();

    printf("************************************************************\n");
    printf("*  TROPHY UNLOCKED!  Trophy #%d                            \n",
           trophyId);
    printf("*  Context: %d  CommId: %s                                 \n",
           context, ctx->commId.data);
    printf("************************************************************\n");

    /* Platinum award (Reference p.48: when the requirements are met as a
     * result of this unlock, the platinum id is returned in *platinumId;
     * otherwise INVALID_TROPHY_ID). The system performs the actual platinum
     * unlock, so record it here too. */
    s32 awarded_platinum = SCE_NP_TROPHY_INVALID_TROPHY_ID;
    if (ctx->platinum_id >= 0 && !ctx->unlocked[ctx->platinum_id] &&
        trophy_platinum_requirements_met(ctx)) {
        ctx->unlocked[ctx->platinum_id] = 1;
        ctx->unlock_time[ctx->platinum_id] = ctx->unlock_time[trophyId];
        awarded_platinum = ctx->platinum_id;
        printf("*  PLATINUM UNLOCKED!  Trophy #%d                          \n",
               ctx->platinum_id);
    }

    /* Save immediately */
    trophy_save(ctx);

    if (platinumId)
        trophy_store_be32(platinumId, (u32)awarded_platinum);

    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    TrophyContext* ctx = &s_contexts[context];

    /* NP_Trophy-Reference p.35: a READ_ONLY context "can use
     * sceNpTrophyGetTrophyUnlockState(). When doing so, there is no need to
     * call sceNpTrophyRegisterContext()." Load config + persisted state on
     * demand for that path; every other context must be registered. */
    if (!ctx->registered) {
        if (!ctx->read_only)
            return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
        trophy_load_state(ctx);
    }

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 words[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32] = {0};
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if (ctx->unlocked[i])
            words[i / 32] |= (1u << (i % 32));
    }
    for (u32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32; i++)
        trophy_store_be32(&flags->flag[i], words[i]);

    /* "count will return the number of valid bits in flags" (Reference
     * p.56) -- the real trophy count when the pack config is present, not
     * the 128-bit array capacity. */
    trophy_store_be32(count, ctx->total_trophies);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 total = s_contexts[context].total_trophies;
    u32 unlocked = trophy_count_unlocked(&s_contexts[context]);

    const s32 result = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    trophy_store_be32(percentage, (u32)result);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n",
           context, result);
    return CELL_OK;
}
