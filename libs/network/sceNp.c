/*
 * ps3recomp - sceNp HLE implementation
 *
 * Offline NP stub: init/term, the NP Manager status reports OFFLINE, and
 * identity and ticket requests return OFFLINE. The non-SDK helpers near the
 * top return a configurable placeholder identity (default "PS3Player").
 */

#include "sceNp.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_np_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, username, SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

/* Build a fake NP ID from the current username */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

s32 sceNpTerm(void)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_score_initialized = 0;
    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpScoreInit(void)
{
    if (s_np_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    if (!s_np_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_score_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    np_build_fake_id(npId);
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    *region = 0x5553;

    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    *age = 25; /* default adult age */
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    langs->language1 = SCE_NP_LANG_ENGLISH;
    langs->language2 = 0;
    langs->language3 = 0;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * We run permanently offline: GetStatus reports OFFLINE so games gate their
 * online flows on it and skip them cleanly, matching how a real signed-out
 * console behaves. GetNpId, GetContentRatingFlag and RequestTicket2 return
 * the offline result; GetTicket reports an empty ticket.
 * -----------------------------------------------------------------------*/

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    *status = SCE_NP_MANAGER_STATUS_OFFLINE;
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!callback)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* We never transition online, so the callback is never stored or
     * invoked. */
    (void)arg;
    return CELL_OK;
}

s32 sceNpManagerGetNpId(SceNpId* npId)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Returns OFFLINE without filling npId when signed out. */
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpManagerGetTicket(void* buffer, u32* bufferSize)
{
    (void)buffer;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!bufferSize)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Not gated on sign-in state on real hardware: the ticket cache is
     * simply empty offline, so report a zero-length ticket either way. */
    *bufferSize = 0;
    return CELL_OK;
}

s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!isRestricted || !age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Returns OFFLINE without filling either output when signed out. */
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpManagerRequestTicket2(const SceNpId* npId, const void* version, const char* serviceId,
                                const void* cookie, u32 cookieSize, const char* entitlementId,
                                u32 consumedCount)
{
    (void)version; (void)cookie; (void)entitlementId; (void)consumedCount;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId || !serviceId || cookieSize > SCE_NP_COOKIE_MAX_SIZE)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Returns OFFLINE (before issuing the ticket request) when signed out. */
    return SCE_NP_ERROR_OFFLINE;
}
