/*
 * ps3recomp - cellGame HLE implementation
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#include "cellGame.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static char s_title_id[64]  = "BLUS30826";  /* fallback; overridden at boot by cellGame_init_from_paramsfo (reads PARAM.SFO) */
static char s_title[256]    = "Unknown Title";
static char s_app_ver[16]   = "01.00";

/* Content info / usrdir paths */
static char s_content_path[CELL_GAME_PATH_MAX] = "./gamedata/dev_hdd0/game";
static char s_content_info_path[CELL_GAME_PATH_MAX] = "";
static char s_usrdir_path[CELL_GAME_PATH_MAX] = "";
static char s_tmp_path[CELL_GAME_PATH_MAX] = "";
static char s_exit_param[256] = "";

static int  s_boot_checked = 0;
static u32  s_game_type = CELL_GAME_GAMETYPE_DISC;

static void ensure_dirs(const char* path)
{
    char tmp[CELL_GAME_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    HOST_MKDIR(tmp);
}

static int dir_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void cellGame_set_title_id(const char* title_id)
{
    if (!title_id) return;
    strncpy(s_title_id, title_id, sizeof(s_title_id) - 1);
    s_title_id[sizeof(s_title_id) - 1] = '\0';
}

void cellGame_set_title(const char* title)
{
    if (!title) return;
    strncpy(s_title, title, sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
}

void cellGame_set_content_path(const char* path)
{
    if (!path) return;
    strncpy(s_content_path, path, sizeof(s_content_path) - 1);
    s_content_path[sizeof(s_content_path) - 1] = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * PARAM.SFO -> title id (2026-06-21): the ROBUST fix for title-id paths.
 *
 * Root cause of the /dev_hdd0/game/BLES00000 bug: s_title_id was a hardcoded
 * placeholder and cellGame_set_title_id() was never called, so EVERY title-id
 * path (cellGameContentPermit/BootCheck/DataCheck/web + cellDiscGameGetBootDiscInfo)
 * used the wrong id. Fix: read the real id from the game's PARAM.SFO once at boot
 * (main.cpp -> cellGame_init_from_paramsfo) so it's correct for ANY title.
 *
 * SFO format is LITTLE-ENDIAN: header @0 (magic "\0PSF", key_table@0x08,
 * data_table@0x0C, entries@0x10), then entries[0x10 each]: key_off(u16)@0,
 * fmt(u16)@2, data_len(u32)@4, data_max(u32)@8, data_off(u32)@0xC.
 * -----------------------------------------------------------------------*/
static int sfo_read_string(const char* sfo_path, const char* key,
                           char* out, int out_size)
{
    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 0x14 || sz > (1 << 20)) { fclose(fp); return -1; }
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    if (!d) { fclose(fp); return -1; }
    int ok = (fread(d, 1, (size_t)sz, fp) == (size_t)sz);
    fclose(fp);
    if (!ok) { free(d); return -1; }

    int ret = -1;
    if (d[0]==0x00 && d[1]==0x50 && d[2]==0x53 && d[3]==0x46) {  /* "\0PSF" */
        #define SFO_RD32(o) ((u32)d[o] | ((u32)d[(o)+1]<<8) | ((u32)d[(o)+2]<<16) | ((u32)d[(o)+3]<<24))
        #define SFO_RD16(o) ((u16)d[o] | ((u16)d[(o)+1]<<8))
        u32 key_tab  = SFO_RD32(0x08);
        u32 data_tab = SFO_RD32(0x0C);
        u32 n        = SFO_RD32(0x10);
        for (u32 i = 0; i < n; i++) {
            u32 e = 0x14 + i * 0x10;
            if (e + 0x10 > (u32)sz) break;
            u32 key_off  = SFO_RD16(e + 0x00);
            u32 data_len = SFO_RD32(e + 0x04);
            u32 data_off = SFO_RD32(e + 0x0C);
            if (key_tab + key_off >= (u32)sz) continue;
            if (strcmp((const char*)(d + key_tab + key_off), key) != 0) continue;
            u32 src = data_tab + data_off;
            if (src >= (u32)sz) break;
            int copy = (int)data_len;
            if (copy > (int)((u32)sz - src)) copy = (int)((u32)sz - src);
            if (copy >= out_size) copy = out_size - 1;
            if (copy < 0) copy = 0;
            memcpy(out, d + src, (size_t)copy);
            out[copy] = '\0';
            ret = 0;
            break;
        }
        #undef SFO_RD32
        #undef SFO_RD16
    }
    free(d);
    return ret;
}

/* Read TITLE_ID / TITLE / APP_VER from the game's PARAM.SFO at boot. Call once
 * from main.cpp before the guest runs. Falls back to the defaults if the SFO
 * can't be read (keeps the game working without it). */
void cellGame_init_from_paramsfo(const char* sfo_path)
{
    char tmp[256];
    if (sfo_read_string(sfo_path, "TITLE_ID", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_title_id, tmp, sizeof(s_title_id) - 1);
        s_title_id[sizeof(s_title_id) - 1] = '\0';
        printf("[cellGame] title id from PARAM.SFO ('%s'): '%s'\n", sfo_path, s_title_id);
    } else {
        printf("[cellGame] PARAM.SFO not read ('%s'); keeping title id '%s'\n",
               sfo_path, s_title_id);
    }
    if (sfo_read_string(sfo_path, "TITLE", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_title, tmp, sizeof(s_title) - 1); s_title[sizeof(s_title) - 1] = '\0';
    }
    if (sfo_read_string(sfo_path, "APP_VER", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_app_ver, tmp, sizeof(s_app_ver) - 1); s_app_ver[sizeof(s_app_ver) - 1] = '\0';
    }
}

/* Central title-id accessor so other modules (cellSysutil etc.) don't hardcode it. */
const char* cellGame_get_title_id(void) { return s_title_id; }

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName)
{
    printf("[cellGame] BootCheck()\n");

    /* Build paths based on title ID */
    snprintf(s_content_info_path, sizeof(s_content_info_path),
             "%s/%s", s_content_path, s_title_id);
    snprintf(s_usrdir_path, sizeof(s_usrdir_path),
             "%s/%s/USRDIR", s_content_path, s_title_id);
    snprintf(s_tmp_path, sizeof(s_tmp_path),
             "%s/%s_TMP", s_content_path, s_title_id);

#ifdef _WIN32
    for (char* p = s_content_info_path; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = s_usrdir_path; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = s_tmp_path; *p; p++) if (*p == '/') *p = '\\';
#endif

    if (type)
        *type = s_game_type;

    if (attributes)
        *attributes = 0;

    if (size) {
        /* hddFreeSizeKB is guest big-endian (see cellGameDataCheck); writing it
         * host-endian made the game read ~4 MB free instead of 1 GB. */
        unsigned char* p = (unsigned char*)&size->hddFreeSizeKB;
        u32 v = 1024u * 1024u; /* 1GB free */
        p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v;
        size->sizeKB = CELL_GAME_SIZEKB_NOTCALC;
        size->sysSizeKB = 0;
    }

    if (dirName) {
        strncpy(dirName, s_title_id, CELL_GAME_PATH_MAX - 1);
        dirName[CELL_GAME_PATH_MAX - 1] = '\0';
    }

    s_boot_checked = 1;
    printf("[cellGame] BootCheck: type=%u, titleId='%s'\n", s_game_type, s_title_id);
    return CELL_OK;
}

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath)
{
    printf("[cellGame] ContentPermit()\n");

    /* Ensure directories exist */
    ensure_dirs(s_content_info_path);
    ensure_dirs(s_usrdir_path);

    if (contentInfoPath) {
        /* Return PS3-style path */
        snprintf(contentInfoPath, CELL_GAME_PATH_MAX,
                 "/dev_hdd0/game/%s", s_title_id);
    }

    if (usrdirPath) {
        snprintf(usrdirPath, CELL_GAME_PATH_MAX,
                 "/dev_hdd0/game/%s/USRDIR", s_title_id);
    }

    return CELL_OK;
}

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
{
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n",
           type, dirName ? dirName : "<null>");

    const char* check_dir = dirName ? dirName : s_title_id;
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, check_dir);

    if (size) {
        /* CellGameContentSize is guest big-endian. hddFreeSizeKB must be written BE
         * (was host-endian -> game read ~4 MB free instead of 1 GB and could refuse).
         * sizeKB(=NOTCALC 0xFFFFFFFF) and sysSizeKB(=0) are byte-invariant. */
        unsigned char* p = (unsigned char*)&size->hddFreeSizeKB;
        u32 v = 1024u * 1024u;
        p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v;
        size->sizeKB = CELL_GAME_SIZEKB_NOTCALC;
        size->sysSizeKB = 0;
    }

    if (dir_exists(path)) {
        return CELL_OK;
    }

    return CELL_GAME_ERROR_NOTFOUND;
}

s32 cellGameGetParamInt(s32 id, s32* value)
{
    printf("[cellGame] GetParamInt(id=%d)\n", id);

    if (!value)
        return CELL_GAME_ERROR_PARAM;

    switch (id) {
    case CELL_GAME_PARAMID_PARENTAL_LEVEL:
        *value = 0;
        break;
    case CELL_GAME_PARAMID_RESOLUTION:
        *value = 0;
        break;
    case CELL_GAME_PARAMID_SOUND_FORMAT:
        *value = 0;
        break;
    case CELL_GAME_PARAMID_APP_VER:
        /* guest big-endian s32 (other cases write 0, which is byte-invariant) */
        { unsigned char* p = (unsigned char*)value; u32 v = 100u; /* 1.00 */
          p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }
        break;
    default:
        printf("[cellGame] WARNING: unknown param int id %d\n", id);
        *value = 0;
        break;
    }

    return CELL_OK;
}

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize)
{
    printf("[cellGame] GetParamString(id=%d, bufsize=%u)\n", id, bufsize);

    if (!buf || bufsize == 0)
        return CELL_GAME_ERROR_PARAM;

    switch (id) {
    case CELL_GAME_PARAMID_TITLE:
    case CELL_GAME_PARAMID_TITLE_DEFAULT:
        strncpy(buf, s_title, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_GAME_PARAMID_TITLE_ID:
        strncpy(buf, s_title_id, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_GAME_PARAMID_APP_VER_STR:
    case CELL_GAME_PARAMID_VERSION:
        strncpy(buf, s_app_ver, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    default:
        printf("[cellGame] WARNING: unknown param string id %d\n", id);
        buf[0] = '\0';
        break;
    }

    return CELL_OK;
}

s32 cellGameCreateGameData(CellGameSetInitParams* init, char* tmp_contentInfoPath,
                            char* tmp_usrdirPath)
{
    printf("[cellGame] CreateGameData()\n");

    if (!init)
        return CELL_GAME_ERROR_PARAM;

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, s_title_id);

    ensure_dirs(path);

    /* Also create USRDIR */
    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    if (tmp_contentInfoPath) {
        strncpy(tmp_contentInfoPath, path, CELL_GAME_PATH_MAX - 1);
        tmp_contentInfoPath[CELL_GAME_PATH_MAX - 1] = '\0';
    }

    if (tmp_usrdirPath) {
        strncpy(tmp_usrdirPath, usrdir, CELL_GAME_PATH_MAX - 1);
        tmp_usrdirPath[CELL_GAME_PATH_MAX - 1] = '\0';
    }

    return CELL_OK;
}

s32 cellGameDeleteGameData(const char* dirName)
{
    printf("[cellGame] DeleteGameData(dir='%s')\n", dirName ? dirName : "<null>");

    if (!dirName)
        return CELL_GAME_ERROR_PARAM;

    /* We won't recursively delete host directories for safety.
       Just report success. The game should handle this gracefully. */
    printf("[cellGame] WARNING: DeleteGameData not performing recursive delete for safety\n");
    return CELL_OK;
}

/* cellGameSetExitParam is implemented in cellGameExec.c with the correct
 * PS3 SDK signature (CellGameExecBootParam*). Not duplicated here. */

s32 cellGameGetSizeKB(s32* sizeKB)
{
    printf("[cellGame] GetSizeKB()\n");

    if (!sizeKB)
        return CELL_GAME_ERROR_PARAM;

    /* Report the content directory's size.
       For now, estimate or return 0. */
    *sizeKB = 0;
    return CELL_OK;
}

s32 cellGameGetLocalWebContentPath(char* path)
{
    printf("[cellGame] GetLocalWebContentPath()\n");

    if (!path)
        return CELL_GAME_ERROR_PARAM;

    snprintf(path, CELL_GAME_PATH_MAX,
             "/dev_hdd0/game/%s/USRDIR/web", s_title_id);

    return CELL_OK;
}
