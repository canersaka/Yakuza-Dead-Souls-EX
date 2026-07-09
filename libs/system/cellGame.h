/*
 * ps3recomp - cellGame HLE
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#ifndef PS3RECOMP_CELL_GAME_H
#define PS3RECOMP_CELL_GAME_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
/* s23 conformance fix: PATH_MAX was 1055 (copy-pasted from the SYSCACHE
 * header's CELL_SYSCACHE_PATH_MAX); the real content-path constant is 128
 * (RPCS3 cellGame.cpp:943 vm::ptr<char[CELL_GAME_PATH_MAX]> with
 * CELL_GAME_PATH_MAX=128) and guest buffers are sized to it -- the old value
 * let our snprintf/strncpy bound writes 8x past the guest's real array. */
#define CELL_GAME_PATH_MAX          128
#define CELL_GAME_DIRNAME_SIZE      32

#define CELL_GAME_GAMETYPE_DISC     1
#define CELL_GAME_GAMETYPE_HDD      2
#define CELL_GAME_GAMETYPE_GAMEDATA 3
#define CELL_GAME_GAMETYPE_HOME     4

/* Boot status */
#define CELL_GAME_ATTRIBUTE_PATCH           (1 << 0)
#define CELL_GAME_ATTRIBUTE_APP_HOME        (1 << 1)
#define CELL_GAME_ATTRIBUTE_DEBUG           (1 << 2)
#define CELL_GAME_ATTRIBUTE_XMBBUY          (1 << 3)
#define CELL_GAME_ATTRIBUTE_COMMERCE2_BROWSER (1 << 4)
#define CELL_GAME_ATTRIBUTE_INVITE_MESSAGE  (1 << 5)
#define CELL_GAME_ATTRIBUTE_CUSTOM_DATA_MESSAGE (1 << 6)
#define CELL_GAME_ATTRIBUTE_WEB_BROWSER     (1 << 8)

/* PARAM.SFO parameter IDs -- s23 conformance fix: this is ONE unified enum
 * (RPCS3 cellGame.h CELL_GAME_PARAMID_*), not disjoint int/string ranges.
 * The old invented scheme (ints 100-104, strings 200-203) made the game's
 * real GetParamString(101 = VERSION, bufsize 6) hit the unknown-id path and
 * return "" every boot (measured s23boot1.out:214). Types: 0/1/100/101/105/
 * 106 are strings; 102/103/104 are ints. */
#define CELL_GAME_PARAMID_TITLE                  0
#define CELL_GAME_PARAMID_TITLE_DEFAULT          1
#define CELL_GAME_PARAMID_TITLE_ID              100
#define CELL_GAME_PARAMID_VERSION               101
#define CELL_GAME_PARAMID_PARENTAL_LEVEL        102
#define CELL_GAME_PARAMID_RESOLUTION            103
#define CELL_GAME_PARAMID_SOUND_FORMAT          104
#define CELL_GAME_PARAMID_PS3_SYSTEM_VER        105
#define CELL_GAME_PARAMID_APP_VER               106

/* Size info mode */
#define CELL_GAME_SIZEKB_NOTCALC    (-1)

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_GAME_ERROR_NOTFOUND       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x01)
#define CELL_GAME_ERROR_BROKEN         (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x02)
#define CELL_GAME_ERROR_INTERNAL       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x03)
#define CELL_GAME_ERROR_PARAM          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x04)
#define CELL_GAME_ERROR_NOAPP          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x05)
#define CELL_GAME_ERROR_ACCESS_ERROR   (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x06)
#define CELL_GAME_ERROR_NOSPACE        (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x07)
#define CELL_GAME_ERROR_NOTSUPPORTED   (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x08)
#define CELL_GAME_ERROR_FAILURE        (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x09)
#define CELL_GAME_ERROR_BUSY           (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0A)
#define CELL_GAME_ERROR_IN_SHUTDOWN    (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0B)
#define CELL_GAME_ERROR_INVALID_ID     (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0C)
#define CELL_GAME_ERROR_EXIST          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0D)
#define CELL_GAME_ERROR_NOTPATCH       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0E)
#define CELL_GAME_ERROR_INVALID_THEME_FILE (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0F)
#define CELL_GAME_ERROR_BOOTPATH       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x10)

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellGameContentSize {
    s32 hddFreeSizeKB;
    s32 sizeKB;
    s32 sysSizeKB;
} CellGameContentSize;

/* Mirrors RPCS3's CellGameSetInitParams (input to cellGameCreateGameData). */
typedef struct CellGameSetInitParams {
    char title[128];
    char titleId[10];
    char reserved0[2];
    char version[6];
    char reserved1[66];
} CellGameSetInitParams;

/* ---------------------------------------------------------------------------
 * Configuration (call before game boots)
 * -----------------------------------------------------------------------*/

/* Set the game's title ID (e.g., "BLUS30443") */
void cellGame_set_title_id(const char* title_id);

/* Read TITLE_ID/TITLE/APP_VER from the game's PARAM.SFO at boot (robust title id
 * for all path building). Falls back to defaults if the SFO can't be read. */
void cellGame_init_from_paramsfo(const char* sfo_path);

/* Central title-id accessor (so other modules don't hardcode placeholders). */
const char* cellGame_get_title_id(void);

/* Set the game's title string */
void cellGame_set_title(const char* title);

/* Set the content info path root */
void cellGame_set_content_path(const char* path);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName);

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath);

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size);

s32 cellGameGetParamInt(s32 id, s32* value);

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize);

s32 cellGameCreateGameData(CellGameSetInitParams* init, char* tmp_contentInfoPath,
                            char* tmp_usrdirPath);

s32 cellGameDeleteGameData(const char* dirName);

/* cellGameSetExitParam — see cellGameExec.h for correct signature */

s32 cellGameGetSizeKB(s32* sizeKB);

s32 cellGameGetLocalWebContentPath(char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GAME_H */
