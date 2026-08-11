/*
 * ps3recomp - cellGame HLE implementation
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#include "cellGame.h"
#include "cellSysutil.h"   /* cellSysutilQueueEvent for the _EXIT dialog types */
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
static char s_version[16]   = "01.00";   /* SFO VERSION (content version, PARAMID 101) */

/* Content info / usrdir paths */
static char s_content_path[CELL_GAME_PATH_MAX] = "./gamedata/dev_hdd0/game";
static char s_content_info_path[CELL_GAME_PATH_MAX] = "";
static char s_usrdir_path[CELL_GAME_PATH_MAX] = "";
static char s_tmp_path[CELL_GAME_PATH_MAX] = "";
static char s_exit_param[256] = "";

static int  s_boot_checked = 0;
static u32  s_game_type = CELL_GAME_GAMETYPE_DISC;

/* Directory name recorded by the most recent access-PREPARING call
 * (cellGameDataCheck / cellGameBootCheck). cellGameContentPermit must emit
 * the path of THAT content, not unconditionally the boot title id
 * (2026-08-05 doc-conformance fix: dirName was ignored, breaking any
 * dirName != TITLE_ID game data). Empty = fall back to the boot title id. */
static char s_prepared_dirname[CELL_GAME_DIRNAME_SIZE] = "";

/* BE store for 32-bit guest out-params. The int members of
 * CellGameContentSize and the type/attributes out-params are guest
 * big-endian; a plain host store flips them (the class of bug fixed for
 * hddFreeSizeKB earlier, and found again on BootCheck *type by the
 * 2026-08-04 audit). */
static void be_store32(void* dst, u32 v)
{
    unsigned char* p = (unsigned char*)dst;
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

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
    if (sfo_read_string(sfo_path, "VERSION", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_version, tmp, sizeof(s_version) - 1); s_version[sizeof(s_version) - 1] = '\0';
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

    /* All 32-bit out-params are guest big-endian. 2026-08-04 audit: *type
     * was stored host-endian, so a guest would read 0x01000000 instead of
     * CELL_GAME_GAMETYPE_DISC. (BootCheck is not imported by this title —
     * latent, fixed for correctness.) */
    if (type)
        be_store32(type, s_game_type);

    if (attributes)
        be_store32(attributes, 0);

    if (size) {
        be_store32(&size->hddFreeSizeKB, 1024u * 1024u);  /* 1GB free */
        /* NOTCALC (-1 = 0xFFFFFFFF) and 0 are byte-invariant, but keep the
         * store discipline uniform. */
        be_store32(&size->sizeKB, (u32)CELL_GAME_SIZEKB_NOTCALC);
        be_store32(&size->sysSizeKB, 0);
    }

    if (dirName) {
        /* s23 conformance fix: dirName is a 32-byte guest array
         * (CELL_GAME_DIRNAME_SIZE, RPCS3 cellGame.cpp:742) -- the old
         * CELL_GAME_PATH_MAX bound (then 1055) let strncpy zero-pad 1054
         * bytes over it. Unreached by this title (BootCheck not imported)
         * but a landmine for any other. */
        strncpy(dirName, s_title_id, CELL_GAME_DIRNAME_SIZE - 1);
        dirName[CELL_GAME_DIRNAME_SIZE - 1] = '\0';
    }

    /* BootCheck is an access-preparing call: the following ContentPermit
     * emits this content's path. */
    strncpy(s_prepared_dirname, s_title_id, sizeof(s_prepared_dirname) - 1);
    s_prepared_dirname[sizeof(s_prepared_dirname) - 1] = '\0';

    s_boot_checked = 1;
    printf("[cellGame] BootCheck: type=%u, titleId='%s'\n", s_game_type, s_title_id);
    return CELL_OK;
}

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath)
{
    /* 2026-08-05 doc-conformance fixes:
     *  - honor the dirName recorded by the preparing call instead of always
     *    emitting the boot title id (identical for this title's
     *    DataCheck(type=3, "BLUS30826") flow, wrong for any other dirName);
     *  - NULL out-params are a documented error, not silently OK
     *    ORACLE(sysutil_gamecontent.h:189-190 "NULL specified is an error";
     *    Reference p.12 CELL_GAME_ERROR_PARAM). NULL survives the import
     *    bridge (yz_hp is NULL-preserving), so the check fires. */
    const char* dir = s_prepared_dirname[0] ? s_prepared_dirname : s_title_id;

    printf("[cellGame] ContentPermit(dir='%s')\n", dir);

    if (!contentInfoPath || !usrdirPath)
        return CELL_GAME_ERROR_PARAM;

    /* Ensure the host-side directories for the prepared content exist. */
    char host_info[CELL_GAME_PATH_MAX];
    char host_usrdir[CELL_GAME_PATH_MAX];
    snprintf(host_info, sizeof(host_info), "%s/%s", s_content_path, dir);
    snprintf(host_usrdir, sizeof(host_usrdir), "%s/USRDIR", host_info);
    ensure_dirs(host_info);
    ensure_dirs(host_usrdir);

    /* Return PS3-style vpaths (guest accesses them through cellFs). */
    snprintf(contentInfoPath, CELL_GAME_PATH_MAX, "/dev_hdd0/game/%s", dir);
    snprintf(usrdirPath, CELL_GAME_PATH_MAX, "/dev_hdd0/game/%s/USRDIR", dir);

    return CELL_GAME_RET_OK;
}

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
{
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n",
           type, dirName ? dirName : "<null>");

    const char* check_dir = dirName ? dirName : s_title_id;
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, check_dir);

    /* Record the prepared dirName: ContentPermit / CreateGameData act on
     * this content (2026-08-05 doc-conformance fix; was ignored). */
    strncpy(s_prepared_dirname, check_dir, sizeof(s_prepared_dirname) - 1);
    s_prepared_dirname[sizeof(s_prepared_dirname) - 1] = '\0';

    if (size) {
        /* CellGameContentSize is guest big-endian. hddFreeSizeKB must be
         * written BE (was host-endian -> game read ~4 MB free instead of
         * 1 GB and could refuse). */
        be_store32(&size->hddFreeSizeKB, 1024u * 1024u);
        be_store32(&size->sizeKB, (u32)CELL_GAME_SIZEKB_NOTCALC);
        be_store32(&size->sysSizeKB, 0);
    }

    if (dir_exists(path)) {
        return CELL_GAME_RET_OK;
    }

    /* Missing content is NOT an error: the documented result is
     * CELL_GAME_RET_NONE(2) ("without handling this as an error",
     * Game_Content-Reference p.9), after which the app is expected to call
     * cellGameCreateGameData. The old CELL_GAME_ERROR_NOTFOUND return sent
     * a fresh install down the game's error path instead (2026-08-04
     * audit HIGH). */
    printf("[cellGame] DataCheck: '%s' absent -> CELL_GAME_RET_NONE\n", path);
    return CELL_GAME_RET_NONE;
}

s32 cellGameGetParamInt(s32 id, s32* value)
{
    printf("[cellGame] GetParamInt(id=%d)\n", id);

    if (!value)
        return CELL_GAME_ERROR_PARAM;

    switch (id) {
    case CELL_GAME_PARAMID_PARENTAL_LEVEL:
    case CELL_GAME_PARAMID_RESOLUTION:
    case CELL_GAME_PARAMID_SOUND_FORMAT:
        /* s23: the unified enum makes these 102/103/104; APP_VER (106) is a
         * STRING param and no longer answered here. 0 is byte-invariant BE. */
        *value = 0;
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
    case CELL_GAME_PARAMID_VERSION:
        /* s23: the game's measured GetParamString(101, 6) is the content
         * VERSION string (SFO "VERSION" key), previously the unknown-id path. */
        strncpy(buf, s_version, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_GAME_PARAMID_APP_VER:
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

/* Minimal PARAM.SFO writer for created game data. The Reference (p.14,
 * CellGameSetInitParams Notes) says the game content utility creates the
 * game data's PARAM.SFO from the init params and the app must not write the
 * file itself -- so our HLE writes it here. Same LITTLE-ENDIAN layout our
 * own reader (sfo_read_string above) parses; fmt 0x0204 = NUL-terminated
 * UTF-8 string. Keys must be in ascending strcmp order. */
static void le_store16(unsigned char* p, u16 v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void le_store32(unsigned char* p, u32 v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }

static int sfo_write_gamedata(const char* dir_path, const CellGameSetInitParams* init)
{
    struct { const char* key; const char* src; u32 src_size; u32 data_max; } e[3] = {
        { "TITLE",    init->title,   (u32)sizeof(init->title),   128 },
        { "TITLE_ID", init->titleId, (u32)sizeof(init->titleId),  16 },
        { "VERSION",  init->version, (u32)sizeof(init->version),   8 },
    };
    enum { N = 3 };
    unsigned char buf[0x14 + N*0x10 + 32 /*keys*/ + 128+16+8 /*data*/ + 8];
    memset(buf, 0, sizeof(buf));

    u32 key_tab = 0x14 + N * 0x10;
    u32 key_off = 0;
    u32 key_offs[N];
    for (int i = 0; i < N; i++) {
        key_offs[i] = key_off;
        size_t klen = strlen(e[i].key) + 1;
        memcpy(buf + key_tab + key_off, e[i].key, klen);
        key_off += (u32)klen;
    }
    u32 data_tab = (key_tab + key_off + 3u) & ~3u;   /* 4-align the data table */

    buf[0]=0x00; buf[1]=0x50; buf[2]=0x53; buf[3]=0x46;  /* "\0PSF" */
    le_store32(buf + 0x04, 0x00000101);                   /* version 1.1 */
    le_store32(buf + 0x08, key_tab);
    le_store32(buf + 0x0C, data_tab);
    le_store32(buf + 0x10, N);

    u32 data_off = 0;
    for (int i = 0; i < N; i++) {
        unsigned char* ent = buf + 0x14 + i * 0x10;
        /* init fields need not be NUL-terminated: bound by the field size. */
        u32 len = 0;
        while (len < e[i].src_size && e[i].src[len]) len++;
        if (len >= e[i].data_max) len = e[i].data_max - 1;
        memcpy(buf + data_tab + data_off, e[i].src, len);  /* +NUL via memset */
        le_store16(ent + 0x00, (u16)key_offs[i]);
        le_store16(ent + 0x02, 0x0204);          /* utf8 string */
        le_store32(ent + 0x04, len + 1);         /* data_len incl. NUL */
        le_store32(ent + 0x08, e[i].data_max);
        le_store32(ent + 0x0C, data_off);
        data_off += e[i].data_max;
    }

    char sfo_path[CELL_GAME_PATH_MAX + 16];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", dir_path);
    FILE* fp = fopen(sfo_path, "wb");
    if (!fp) return -1;
    size_t total = data_tab + data_off;
    int ok = fwrite(buf, 1, total, fp) == total;
    fclose(fp);
    return ok ? 0 : -1;
}

s32 cellGameCreateGameData(CellGameSetInitParams* init, char* tmp_contentInfoPath,
                            char* tmp_usrdirPath)
{
    if (!init)
        return CELL_GAME_ERROR_PARAM;

    /* The game data directory is the dirName given to the preceding
     * cellGameDataCheck (Reference p.15: "Call this function after the call
     * of cellGameDataCheck()"), not the boot title id. */
    const char* dir = s_prepared_dirname[0] ? s_prepared_dirname : s_title_id;

    printf("[cellGame] CreateGameData(dir='%s', titleId='%.*s', title='%.*s', version='%.*s')\n",
           dir,
           (int)sizeof(init->titleId), init->titleId,
           (int)sizeof(init->title),   init->title,
           (int)sizeof(init->version), init->version);

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, dir);
    ensure_dirs(path);

    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    /* Honor the init params: the utility (us) creates the game data's
     * PARAM.SFO from them (Reference p.14). */
    if (sfo_write_gamedata(path, init) != 0)
        printf("[cellGame] WARNING: could not write %s/PARAM.SFO\n", path);

    /* 2026-08-05 doc-conformance fix: the out-paths are guest VPATHS that
     * must resolve through cellFs, not host-relative "./gamedata/..."
     * strings (which our own cellFs translation cannot resolve -- 2026-08-04
     * audit HIGH). NULL out-params are documented "not set" for this entry
     * ORACLE(sysutil_gamecontent.h:156-157). Deliberate deviation: no
     * temp+rename staging -- we create the final directory directly and
     * return its (final) path as the "temporary" path; ContentPermit then
     * returns the same path, which the Reference explicitly allows the app
     * to use immediately (p.15). */
    if (tmp_contentInfoPath)
        snprintf(tmp_contentInfoPath, CELL_GAME_PATH_MAX, "/dev_hdd0/game/%s", dir);

    if (tmp_usrdirPath)
        snprintf(tmp_usrdirPath, CELL_GAME_PATH_MAX, "/dev_hdd0/game/%s/USRDIR", dir);

    return CELL_GAME_RET_OK;
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

s32 cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName)
{
    /* Minimal HLE (2026-08-05, was an unimplemented-import CELL_ENOSYS
     * stub -- an out-of-contract negative; NID 0xB0A1F8C6 IS imported by
     * this title). Mirrors cellMsgDialog's convention: log the dialog, no
     * UI, resolve it immediately.
     *
     * Contract ORACLE(Game_Content-Reference pp.31-33): returns
     * CELL_GAME_RET_OK(0) after the dialog closes; for the _EXIT types
     * (100-102) the utility additionally issues the game-termination
     * request event AFTER this function returns -- "enable this function to
     * return and then stop to wait for the issuance of the game termination
     * request event". We queue CELL_SYSUTIL_REQUEST_EXITGAME on the sysutil
     * event queue, which cellSysutilCheckCallback delivers on the game's
     * next poll (i.e. after we return) -- the path the old ENOSYS hang
     * analysis identified as missing. */
    const char* what;
    switch (type) {
    case CELL_GAME_ERRDIALOG_BROKEN_GAMEDATA:
    case CELL_GAME_ERRDIALOG_BROKEN_EXIT_GAMEDATA:
        what = "game data is corrupted";
        break;
    case CELL_GAME_ERRDIALOG_BROKEN_HDDGAME:
    case CELL_GAME_ERRDIALOG_BROKEN_EXIT_HDDGAME:
        what = "HDD boot game is corrupted";
        break;
    case CELL_GAME_ERRDIALOG_NOSPACE:
    case CELL_GAME_ERRDIALOG_NOSPACE_EXIT:
        what = "not enough available HDD space";
        break;
    default:
        /* Undefined type: argument error (Reference p.32). */
        printf("[cellGame] ContentErrorDialog: invalid type %d -> PARAM\n", type);
        return CELL_GAME_ERROR_PARAM;
    }

    int exit_requested = (type >= CELL_GAME_ERRDIALOG_BROKEN_EXIT_GAMEDATA);

    printf("========================================\n");
    printf("[cellGame] CONTENT ERROR DIALOG: %s%s\n", what,
           exit_requested ? " (application will be terminated)" : "");
    if (type == CELL_GAME_ERRDIALOG_NOSPACE || type == CELL_GAME_ERRDIALOG_NOSPACE_EXIT)
        printf("[cellGame]   lacking space: %d KB\n", errNeedSizeKB);
    if (dirName)
        printf("[cellGame]   content dir: '%.*s'\n", CELL_GAME_DIRNAME_SIZE, dirName);
    printf("========================================\n");

    if (exit_requested) {
        /* Broadcast to every callback slot; CheckCallback skips the
         * unregistered ones. */
        for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; slot++)
            cellSysutilQueueEvent(slot, CELL_SYSUTIL_REQUEST_EXITGAME, 0);
    }

    return CELL_GAME_RET_OK;
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
