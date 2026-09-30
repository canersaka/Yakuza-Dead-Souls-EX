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
/* Guest path buffers are CELL_GAME_PATH_MAX bytes; bound all copies to it. */
#define CELL_GAME_PATH_MAX          128
#define CELL_GAME_DIRNAME_SIZE      32

#define CELL_GAME_GAMETYPE_DISC     1
#define CELL_GAME_GAMETYPE_HDD      2
#define CELL_GAME_GAMETYPE_GAMEDATA 3
#define CELL_GAME_GAMETYPE_HOME     4

/* Content attribute flags */
#define CELL_GAME_ATTRIBUTE_PATCH           (1 << 0)
#define CELL_GAME_ATTRIBUTE_APP_HOME        (1 << 1)
#define CELL_GAME_ATTRIBUTE_DEBUG           (1 << 2)
#define CELL_GAME_ATTRIBUTE_XMBBUY          (1 << 3)
#define CELL_GAME_ATTRIBUTE_COMMERCE2_BROWSER (1 << 4)
#define CELL_GAME_ATTRIBUTE_INVITE_MESSAGE  (1 << 5)
#define CELL_GAME_ATTRIBUTE_CUSTOM_DATA_MESSAGE (1 << 6)
#define CELL_GAME_ATTRIBUTE_WEB_BROWSER     (1 << 8)

/* PARAM.SFO parameter IDs: one unified enum, not disjoint int/string ranges
 * (GetParamString(101 = VERSION) is a real call). Types: 0/1/100/101/105/106
 * are strings; 102/103/104 are ints. */
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

/* Error dialog types (cellGameContentErrorDialog): the _EXIT variants
 * additionally issue the game-termination request event after the dialog
 * closes. */
#define CELL_GAME_ERRDIALOG_BROKEN_GAMEDATA        0
#define CELL_GAME_ERRDIALOG_BROKEN_HDDGAME         1
#define CELL_GAME_ERRDIALOG_NOSPACE                2
#define CELL_GAME_ERRDIALOG_BROKEN_EXIT_GAMEDATA   100
#define CELL_GAME_ERRDIALOG_BROKEN_EXIT_HDDGAME    101
#define CELL_GAME_ERRDIALOG_NOSPACE_EXIT           102

/* ---------------------------------------------------------------------------
 * Return / error codes
 *
 * The module base is 0x8002cb00 and the offsets are sparse (04-09, 20-28,
 * 50), so the values cannot be derived from a base|n scheme. RET_* are
 * documented non-error positive results (DataCheck returns RET_NONE(2) for
 * missing content).
 * -----------------------------------------------------------------------*/
#define CELL_GAME_RET_OK               (0)
#define CELL_GAME_RET_CANCEL           (1)   /* operation canceled */
#define CELL_GAME_RET_NONE             (2)   /* content does not exist */

#define CELL_GAME_ERROR_NOTFOUND       (s32)0x8002cb04
#define CELL_GAME_ERROR_BROKEN         (s32)0x8002cb05
#define CELL_GAME_ERROR_INTERNAL       (s32)0x8002cb06
#define CELL_GAME_ERROR_PARAM          (s32)0x8002cb07
#define CELL_GAME_ERROR_NOAPP          (s32)0x8002cb08
#define CELL_GAME_ERROR_ACCESS_ERROR   (s32)0x8002cb09
#define CELL_GAME_ERROR_NOSPACE        (s32)0x8002cb20
#define CELL_GAME_ERROR_NOTSUPPORTED   (s32)0x8002cb21
#define CELL_GAME_ERROR_FAILURE        (s32)0x8002cb22
#define CELL_GAME_ERROR_BUSY           (s32)0x8002cb23
#define CELL_GAME_ERROR_IN_SHUTDOWN    (s32)0x8002cb24
#define CELL_GAME_ERROR_INVALID_ID     (s32)0x8002cb25
#define CELL_GAME_ERROR_EXIST          (s32)0x8002cb26
#define CELL_GAME_ERROR_NOTPATCH       (s32)0x8002cb27
#define CELL_GAME_ERROR_INVALID_THEME_FILE (s32)0x8002cb28
#define CELL_GAME_ERROR_BOOTPATH       (s32)0x8002cb50

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellGameContentSize {
    s32 hddFreeSizeKB;
    s32 sizeKB;
    s32 sysSizeKB;
} CellGameContentSize;

/* CellGameSetInitParams (input to cellGameCreateGameData). */
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

/* Read TITLE_ID/TITLE/APP_VER from the game's PARAM.SFO at boot (robust title id
 * for all path building). Falls back to defaults if the SFO can't be read. */
void cellGame_init_from_paramsfo(const char* sfo_path);

/* Central title-id accessor (so other modules don't hardcode placeholders). */
const char* cellGame_get_title_id(void);

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

/* Fatal-content error dialog. Logs the template message, posts the
 * game-termination request event for the _EXIT types, returns
 * CELL_GAME_RET_OK. No UI. */
s32 cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName);

/* cellGameSetExitParam: see cellGameExec.h for the signature */

s32 cellGameGetSizeKB(s32* sizeKB);

s32 cellGameGetLocalWebContentPath(char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GAME_H */
