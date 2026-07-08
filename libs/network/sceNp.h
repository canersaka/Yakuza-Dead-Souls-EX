/*
 * ps3recomp - sceNp HLE
 *
 * PlayStation Network core: NP IDs, online IDs, user profiles.
 */

#ifndef PS3RECOMP_SCE_NP_H
#define PS3RECOMP_SCE_NP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_ERROR_NOT_INITIALIZED            0x80550001
#define SCE_NP_ERROR_ALREADY_INITIALIZED        0x80550002
#define SCE_NP_ERROR_INVALID_ARGUMENT           0x80550003
#define SCE_NP_ERROR_OUT_OF_MEMORY              0x80550004
#define SCE_NP_ERROR_ID_NOT_FOUND               0x80550005
#define SCE_NP_ERROR_SIGNED_OUT                 0x80550006
#define SCE_NP_ERROR_ABORTED                    0x80550007
#define SCE_NP_ERROR_OFFLINE                    0x80550008
#define SCE_NP_ERROR_VARIANT                    0x80550009
#define SCE_NP_ERROR_UNKNOWN                    0x805500FF

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_ONLINEID_MAX_LENGTH              16
#define SCE_NP_ONLINENAME_MAX_LENGTH            48
#define SCE_NP_AVATAR_URL_MAX_LENGTH            127
#define SCE_NP_ABOUT_ME_MAX_LENGTH              63
#define SCE_NP_COMMUNICATION_ID_MAX_LENGTH      9
#define SCE_NP_COMMUNICATION_PASSPHRASE_SIZE    128
#define SCE_NP_COMMUNICATION_SIGNATURE_SIZE     160
#define SCE_NP_COOKIE_MAX_SIZE                  1024

/* Country codes */
#define SCE_NP_LANG_ENGLISH                     1

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct SceNpCommunicationId {
    char data[SCE_NP_COMMUNICATION_ID_MAX_LENGTH];
    char term;
    u8   num;
    u8   padding[3];
} SceNpCommunicationId;

typedef struct SceNpCommunicationPassphrase {
    u8 data[SCE_NP_COMMUNICATION_PASSPHRASE_SIZE];
} SceNpCommunicationPassphrase;

typedef struct SceNpCommunicationSignature {
    u8 data[SCE_NP_COMMUNICATION_SIGNATURE_SIZE];
} SceNpCommunicationSignature;

typedef struct SceNpOnlineId {
    char data[SCE_NP_ONLINEID_MAX_LENGTH];
    char term;
    char padding[3];
} SceNpOnlineId;

typedef struct SceNpId {
    SceNpOnlineId handle;
    u8            opt[8];
    u8            reserved[8];
} SceNpId;

typedef struct SceNpOnlineName {
    char data[SCE_NP_ONLINENAME_MAX_LENGTH];
} SceNpOnlineName;

typedef struct SceNpAvatarUrl {
    char data[SCE_NP_AVATAR_URL_MAX_LENGTH + 1];
} SceNpAvatarUrl;

typedef struct SceNpUserInfo {
    SceNpId         npId;
    SceNpOnlineName onlineName;
    SceNpAvatarUrl  avatarUrl;
} SceNpUserInfo;

typedef struct SceNpMyLanguages {
    u32 language1;
    u32 language2;
    u32 language3;
} SceNpMyLanguages;

/* --- NP Manager (sign-in state). We run permanently offline (no live PSN
 * session): GetStatus reports OFFLINE so games cleanly skip online flows
 * instead of blocking on sign-in, and the identity/ticket getters mirror
 * RPCS3's offline branch (Emu/Cell/Modules/sceNp.cpp), which early-returns
 * SCE_NP_ERROR_OFFLINE without touching its output params whenever
 * np_handler::get_psn_status() == OFFLINE. The status enum value below (-1)
 * matches RPCS3 exactly; the SCE_NP_ERROR_* codes above are this repo's own
 * existing numbering (kept for consistency with the rest of this file's
 * error paths, not Sony's real 0x8002aaXX values). --- */
#define SCE_NP_MANAGER_STATUS_OFFLINE          (-1)
#define SCE_NP_MANAGER_STATUS_GETTING_TICKET    0
#define SCE_NP_MANAGER_STATUS_GETTING_PROFILE   1
#define SCE_NP_MANAGER_STATUS_LOGGING_IN        2
#define SCE_NP_MANAGER_STATUS_ONLINE            3

typedef void (*SceNpManagerCallback)(s32 event, s32 result, void* arg);

s32 sceNpManagerGetStatus(s32* status);
s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg);
s32 sceNpManagerGetNpId(SceNpId* npId);
s32 sceNpManagerGetTicket(void* buffer, u32* bufferSize);
s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age);
s32 sceNpManagerRequestTicket2(const SceNpId* npId, const void* version, const char* serviceId,
                                const void* cookie, u32 cookieSize, const char* entitlementId,
                                u32 consumedCount);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr);
s32 sceNpTerm(void);

s32 sceNpGetNpId(s32 userId, SceNpId* npId);
s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId);
s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName);
s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo);
s32 sceNpGetAccountRegion(s32 userId, u32* region);
s32 sceNpGetAccountAge(s32 userId, s32* age);
s32 sceNpGetMyLanguages(SceNpMyLanguages* langs);

/* Set the fake PSN username (call before sceNpInit if desired) */
void sceNpSetFakeUsername(const char* username);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_H */
