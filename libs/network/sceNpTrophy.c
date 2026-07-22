/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file at gamedata/trophies/{commId}.json.
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 */

#include "sceNpTrophy.h"
#include "ps3emu/guest_call.h"
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
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
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
                ctx->unlock_time[tid] = ts;
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
    (void)commSign; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            trophy_store_be32(context, (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n",
                   commId->data, i);
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

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               SceNpTrophyStatusCallback statusCb,
                               void* callbackArg,
                               u64 options)
{
    (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_REG;

    /* Load any previously saved trophy data */
    trophy_load(&s_contexts[context]);
    s_contexts[context].registered = 1;

    /* Registration is synchronous here, but callers still require the status
     * callback that announces completion. The callback value is a guest OPD. */
    uint32_t status_opd = (uint32_t)(uintptr_t)statusCb;
    if (status_opd && g_ps3_guest_caller) {
        g_ps3_guest_caller(status_opd,
                           (uint64_t)context,
                           SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                           0, 0,
                           (uint32_t)(uintptr_t)callbackArg,
                           0, 0, 0);
    }

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d) -> loaded saved data\n",
           context, handle);
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

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyGameDetails));
        trophy_store_be32(&details->numTrophies, 32); /* default trophy set */
        trophy_store_be32(&details->numPlatinum, 1);
        trophy_store_be32(&details->numGold, 2);
        trophy_store_be32(&details->numSilver, 8);
        trophy_store_be32(&details->numBronze, 21);
        strncpy(details->title, "PS3 Game",
                SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE - 1);
        strncpy(details->description, "Trophy set",
                SCE_NP_TROPHY_GAME_DESC_MAX_SIZE - 1);
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyGameData));
        const u32 unlocked = trophy_count_unlocked(&s_contexts[context]);
        trophy_store_be32(&data->unlockedTrophies, unlocked);
        /* Count by grade would require per-trophy grade data;
         * return conservative estimates */
        trophy_store_be32(&data->unlockedPlatinum, 0);
        trophy_store_be32(&data->unlockedGold, 0);
        trophy_store_be32(&data->unlockedSilver, 0);
        trophy_store_be32(&data->unlockedBronze, unlocked);
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
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyDetails));
        trophy_store_be32(&details->trophyId, (u32)trophyId);
        trophy_store_be32(&details->trophyGrade, SCE_NP_TROPHY_GRADE_BRONZE);
        snprintf(details->name, SCE_NP_TROPHY_NAME_MAX_SIZE,
                 "Trophy %d", trophyId);
        snprintf(details->description, SCE_NP_TROPHY_DESC_MAX_SIZE,
                 "Trophy #%d description", trophyId);
        details->hidden = 0;
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyData));
        trophy_store_be32(&data->trophyId, (u32)trophyId);
        data->unlocked = s_contexts[context].unlocked[trophyId];
        trophy_store_be64(&data->timestamp, s_contexts[context].unlock_time[trophyId]);
    }

    return CELL_OK;
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

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (s_contexts[context].unlocked[trophyId])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    /* Unlock the trophy */
    s_contexts[context].unlocked[trophyId] = 1;
    s_contexts[context].unlock_time[trophyId] = (u64)time(NULL);

    printf("************************************************************\n");
    printf("*  TROPHY UNLOCKED!  Trophy #%d                            \n",
           trophyId);
    printf("*  Context: %d  CommId: %s                                 \n",
           context, s_contexts[context].commId.data);
    printf("************************************************************\n");

    /* Save immediately */
    trophy_save(&s_contexts[context]);

    /* Check if all non-platinum trophies are unlocked -> platinum */
    if (platinumId) {
        trophy_store_be32(platinumId, (u32)SCE_NP_TROPHY_INVALID_TROPHY_ID);
        /* Simplified: don't auto-award platinum without grade info */
    }

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

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 words[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32] = {0};
    for (u32 i = 0; i < s_contexts[context].total_trophies; i++) {
        if (s_contexts[context].unlocked[i])
            words[i / 32] |= (1u << (i % 32));
    }
    for (u32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32; i++)
        trophy_store_be32(&flags->flag[i], words[i]);

    trophy_store_be32(count, s_contexts[context].total_trophies);
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
