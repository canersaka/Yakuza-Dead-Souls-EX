/*
 * ps3recomp - cellSaveData HLE implementation
 *
 * Game save data management with callback-driven flow.
 * Save data is stored under: {root}/gamedata/dev_hdd0/home/00000001/savedata/{dirName}/
 */

#include "cellSaveData.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <strings.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static char s_save_root[1024] = "./gamedata/dev_hdd0/home/00000001/savedata";

/* Ensure directory (and parents) exist */
static void ensure_dirs(const char* path)
{
    char tmp[1024];
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

static void build_save_path(char* buf, size_t buf_size, const char* dirName)
{
    snprintf(buf, buf_size, "%s/%s", s_save_root, dirName);
#ifdef _WIN32
    for (char* p = buf; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif
}

/* dirNamePrefix may hold multiple '|'-separated prefixes (Save_Data-Reference
 * p.5, p.100); a directory matches if it starts with ANY of them. The old
 * single strncmp compared the whole "A|B" string literally and enumerated
 * nothing -- and this enumeration feeds cellSaveDataListAutoLoad, the New
 * Game gate (2026-08-04 doc-conformance audit). Defined below the file-op
 * helpers; declared here for the enumerators. */
static int savedata_prefix_match(const char* name, const char* prefix);

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
 * Guest-callback marshalling
 *
 * The funcStat / funcFile callbacks the game registers are GUEST function
 * pointers (OPD addresses), not host-callable. We have to:
 *   1. Allocate guest memory for the StatGet/StatSet/CBResult structures
 *   2. Write the structs in big-endian format
 *   3. Dispatch the OPD via g_ps3_guest_caller with the guest pointers
 *   4. Read the structures back (game may have updated CBResult.result)
 *
 * Layout (sizes in PS3 ABI, all big-endian):
 *   CellSaveDataCBResult:    20 bytes
 *   CellSaveDataStatGet:     1704 bytes
 *   CellSaveDataStatSet:     12 bytes
 *
 * We use a fixed scratch region at 0x024E0000 (128 KB, just before the
 * cmdbuf region at 0x02500000). Only one savedata operation is in flight
 * at a time, so a bump allocator is sufficient.
 * -----------------------------------------------------------------------*/

#define SAVEDATA_SCRATCH_BASE  0x024E0000u
#define SAVEDATA_SCRATCH_SIZE  0x00020000u  /* 128 KB */

static uint32_t s_scratch_next = SAVEDATA_SCRATCH_BASE;

static void scratch_reset(void) { s_scratch_next = SAVEDATA_SCRATCH_BASE; }

static uint32_t scratch_alloc(uint32_t size)
{
    /* 16-byte align */
    size = (size + 0xF) & ~0xFu;
    if (s_scratch_next + size > SAVEDATA_SCRATCH_BASE + SAVEDATA_SCRATCH_SIZE)
        return 0;
    uint32_t addr = s_scratch_next;
    s_scratch_next += size;
    /* Zero-init */
    memset(vm_base + addr, 0, size);
    return addr;
}

/* Big-endian struct field writers. Offsets are in PS3 ABI layout. */
static void marshal_cbresult_init(uint32_t addr, s32 result, uint32_t userdata)
{
    vm_write32(addr + 0,  (uint32_t)result);     /* result */
    vm_write32(addr + 4,  0);                    /* progressBarInc */
    vm_write32(addr + 8,  0);                    /* errNeedSizeKB */
    vm_write32(addr + 12, 0);                    /* invalidMsg */
    vm_write32(addr + 16, userdata);             /* userdata */
}

#define SAVEDATA_MAX_SECURE_FILES CELL_SAVEDATA_SECUREFILE_MAX

typedef struct SaveDataMetadata {
    CellSaveDataSystemFileParam param;
    char secure_files[SAVEDATA_MAX_SECURE_FILES][CELL_SAVEDATA_FILENAME_SIZE];
    u32 secure_file_count;
    int valid;
} SaveDataMetadata;

typedef struct SaveDataDiskStats {
    s64 atime;
    s64 mtime;
    s64 ctime;
    u64 total_bytes;
    u64 system_bytes;
    u32 file_count;
} SaveDataDiskStats;

static u16 read_le16(const unsigned char* p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 read_le32(const unsigned char* p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void write_le16(unsigned char* p, u16 value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void write_le32(unsigned char* p, u32 value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static int checked_range(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static int is_system_file(const char* name)
{
#ifdef _WIN32
    return _stricmp(name, "PARAM.SFO") == 0 || _stricmp(name, "PARAM.PFD") == 0;
#else
    return strcasecmp(name, "PARAM.SFO") == 0 || strcasecmp(name, "PARAM.PFD") == 0;
#endif
}

static int is_safe_component(const char* name, size_t max_len)
{
    size_t len;
    if (!name || !name[0])
        return 0;
    len = strnlen(name, max_len + 1);
    if (len == 0 || len > max_len || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':')
            return 0;
    }
    return 1;
}

static int metadata_is_secure(const SaveDataMetadata* meta, const char* name)
{
    if (!meta || !name)
        return 0;
    for (u32 i = 0; i < meta->secure_file_count; ++i) {
#ifdef _WIN32
        if (_stricmp(meta->secure_files[i], name) == 0)
#else
        if (strcasecmp(meta->secure_files[i], name) == 0)
#endif
            return 1;
    }
    return 0;
}

static void metadata_set_secure(SaveDataMetadata* meta, const char* name, int secure)
{
    if (!meta || !is_safe_component(name, CELL_SAVEDATA_FILENAME_SIZE - 1))
        return;
    for (u32 i = 0; i < meta->secure_file_count; ++i) {
#ifdef _WIN32
        int match = _stricmp(meta->secure_files[i], name) == 0;
#else
        int match = strcasecmp(meta->secure_files[i], name) == 0;
#endif
        if (!match)
            continue;
        if (!secure) {
            --meta->secure_file_count;
            if (i != meta->secure_file_count)
                memcpy(meta->secure_files[i], meta->secure_files[meta->secure_file_count],
                       CELL_SAVEDATA_FILENAME_SIZE);
        }
        return;
    }
    if (secure && meta->secure_file_count < SAVEDATA_MAX_SECURE_FILES) {
        strncpy(meta->secure_files[meta->secure_file_count], name,
                CELL_SAVEDATA_FILENAME_SIZE - 1);
        meta->secure_files[meta->secure_file_count][CELL_SAVEDATA_FILENAME_SIZE - 1] = '\0';
        ++meta->secure_file_count;
    }
}

static u32 classify_file(const SaveDataMetadata* meta, const char* name)
{
#ifdef _WIN32
#define SAVEDATA_NAME_EQ(a, b) (_stricmp((a), (b)) == 0)
#else
#define SAVEDATA_NAME_EQ(a, b) (strcasecmp((a), (b)) == 0)
#endif
    if (SAVEDATA_NAME_EQ(name, "ICON0.PNG")) return CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
    if (SAVEDATA_NAME_EQ(name, "ICON1.PAM")) return CELL_SAVEDATA_FILETYPE_CONTENT_ICON1;
    if (SAVEDATA_NAME_EQ(name, "PIC1.PNG"))  return CELL_SAVEDATA_FILETYPE_CONTENT_PIC1;
    if (SAVEDATA_NAME_EQ(name, "SND0.AT3"))  return CELL_SAVEDATA_FILETYPE_CONTENT_SND0;
    if (metadata_is_secure(meta, name))      return CELL_SAVEDATA_FILETYPE_SECUREFILE;
    return CELL_SAVEDATA_FILETYPE_NORMALFILE;
#undef SAVEDATA_NAME_EQ
}

static int read_file_fully(const char* path, unsigned char** out_data, size_t* out_size)
{
    FILE* fp;
    long length;
    unsigned char* data;
    *out_data = NULL;
    *out_size = 0;
    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    if (fseek(fp, 0, SEEK_END) != 0 || (length = ftell(fp)) < 0 ||
        length > 16 * 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    data = (unsigned char*)malloc((size_t)length ? (size_t)length : 1);
    if (!data || ((size_t)length && fread(data, 1, (size_t)length, fp) != (size_t)length)) {
        free(data);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *out_data = data;
    *out_size = (size_t)length;
    return 1;
}

static void copy_psf_string(char* dst, size_t dst_size,
                            const unsigned char* src, size_t src_size)
{
    size_t len = 0;
    if (!dst_size)
        return;
    while (len < src_size && src[len])
        ++len;
    if (len >= dst_size)
        len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* PARAM.SFO is a little-endian PSF container even on the big-endian PPU.
 * RPCS3 stores secure-file classification as integer entries named
 * "*<filename>"; the host files themselves are already decrypted. */
static int parse_binary_param_sfo(const unsigned char* data, size_t size,
                                  SaveDataMetadata* meta)
{
    u32 key_table, data_table, count;
    if (!data || size < 20 || memcmp(data, "\0PSF", 4) != 0 ||
        read_le32(data + 4) != 0x101)
        return 0;
    key_table = read_le32(data + 8);
    data_table = read_le32(data + 12);
    count = read_le32(data + 16);
    if (count > 4096 || key_table < 20 || key_table > data_table ||
        data_table > size || !checked_range(20, (size_t)count * 16, size))
        return 0;

    for (u32 i = 0; i < count; ++i) {
        const unsigned char* def = data + 20 + (size_t)i * 16;
        u16 key_off = read_le16(def);
        u16 format = read_le16(def + 2);
        u32 param_len = read_le32(def + 4);
        u32 param_max = read_le32(def + 8);
        u32 value_off = read_le32(def + 12);
        size_t key_pos = (size_t)key_table + key_off;
        size_t value_pos = (size_t)data_table + value_off;
        size_t key_end;
        const char* key;
        if (param_len > param_max || key_pos >= data_table ||
            !checked_range(value_pos, param_len, size))
            return 0;
        key_end = key_pos;
        while (key_end < data_table && data[key_end])
            ++key_end;
        if (key_end == data_table)
            return 0;
        key = (const char*)(data + key_pos);
        if ((format == 0x0204 || format == 0x0004)) {
            if (strcmp(key, "TITLE") == 0)
                copy_psf_string(meta->param.title, sizeof(meta->param.title),
                                data + value_pos, param_len);
            else if (strcmp(key, "SUB_TITLE") == 0)
                copy_psf_string(meta->param.subTitle, sizeof(meta->param.subTitle),
                                data + value_pos, param_len);
            else if (strcmp(key, "DETAIL") == 0)
                copy_psf_string(meta->param.detail, sizeof(meta->param.detail),
                                data + value_pos, param_len);
            else if (strcmp(key, "SAVEDATA_LIST_PARAM") == 0)
                copy_psf_string(meta->param.listParam, sizeof(meta->param.listParam),
                                data + value_pos, param_len);
        } else if (format == 0x0404 && param_len == 4) {
            u32 value = read_le32(data + value_pos);
            if (strcmp(key, "ATTRIBUTE") == 0)
                meta->param.attribute = value;
            else if (key[0] == '*' && value)
                metadata_set_secure(meta, key + 1, 1);
        }
    }
    meta->valid = 1;
    return 1;
}

static int parse_legacy_param_sfo(const unsigned char* data, size_t size,
                                  SaveDataMetadata* meta)
{
    char* text;
    char* line;
    char* next;
    if (!data || !size || memchr(data, '\0', size))
        return 0;
    text = (char*)malloc(size + 1);
    if (!text)
        return 0;
    memcpy(text, data, size);
    text[size] = '\0';
    line = text;
    while (line && *line) {
        next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
            while (*next == '\r' || *next == '\n') ++next;
        }
        if (strncmp(line, "TITLE=", 6) == 0)
            strncpy(meta->param.title, line + 6, sizeof(meta->param.title) - 1);
        else if (strncmp(line, "SUB_TITLE=", 10) == 0)
            strncpy(meta->param.subTitle, line + 10, sizeof(meta->param.subTitle) - 1);
        else if (strncmp(line, "DETAIL=", 7) == 0)
            strncpy(meta->param.detail, line + 7, sizeof(meta->param.detail) - 1);
        else if (strncmp(line, "ATTRIBUTE=", 10) == 0)
            meta->param.attribute = (u32)strtoul(line + 10, NULL, 10);
        else if (strncmp(line, "LIST_PARAM=", 11) == 0)
            strncpy(meta->param.listParam, line + 11, sizeof(meta->param.listParam) - 1);
        line = next;
    }
    free(text);
    meta->valid = meta->param.title[0] != '\0';
    return meta->valid;
}

static int read_param_sfo(const char* save_path, SaveDataMetadata* meta)
{
    char path[1024];
    unsigned char* data;
    size_t size;
    memset(meta, 0, sizeof(*meta));
    snprintf(path, sizeof(path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = path; *p; ++p) if (*p == '/') *p = '\\';
#endif
    if (!read_file_fully(path, &data, &size))
        return 0;
    if (!parse_binary_param_sfo(data, size, meta))
        parse_legacy_param_sfo(data, size, meta);
    free(data);
    return meta->valid;
}

static void marshal_cbresult_reset(uint32_t addr, s32 result)
{
    vm_write32(addr + 0,  (uint32_t)result);
    vm_write32(addr + 4,  0);
    vm_write32(addr + 8,  0);
    vm_write32(addr + 12, 0);
}

static s32 marshal_cbresult_read_result(uint32_t addr)
{
    return (s32)vm_read32(addr + 0);
}

/* PS3 ABI layout of CellSaveDataStatGet (all big-endian on PPC, 32-bit pointers):
 *   s32 hddFreeSizeKB                     +0
 *   u32 isNewData                         +4
 *   CellSaveDataDirStat dir               +8   (s64*3 + char[32] = 56 bytes)
 *   CellSaveDataSystemFileParam getParam  +64  (128+128+1024+4+4+8+256 = 1552 bytes)
 *   u32 bind                              +1616
 *   s32 sizeKB                            +1620
 *   s32 sysSizeKB                         +1624
 *   u32 fileNum                           +1628
 *   u32 fileListNum                       +1632
 *   ptr fileList                          +1636 (32-bit guest ptr)
 *   reserved[64]                          +1640
 *   total                                 1704 bytes
 */
#define SAVEDATA_LISTGET_SIZE   76u
#define SAVEDATA_LISTSET_SIZE   24u
#define SAVEDATA_FIXEDSET_SIZE  12u
#define SAVEDATA_STATGET_SIZE   1704u
#define SAVEDATA_STATSET_SIZE   12u
#define SAVEDATA_FILEGET_SIZE   68u
#define SAVEDATA_FILESET_SIZE   48u
#define SAVEDATA_CBRESULT_SIZE  20u

static s32 bytes_to_kb(u64 bytes)
{
    u64 kb = (bytes + 1023u) / 1024u;
    return kb > INT32_MAX ? INT32_MAX : (s32)kb;
}

static void marshal_statget_init(uint32_t addr, int is_new, const char* dirName,
                                 const SaveDataMetadata* meta,
                                 const SaveDataDiskStats* stats)
{
    vm_write32(addr + 0,    1024 * 1024);    /* hddFreeSizeKB = 1 GB */
    vm_write32(addr + 4,    is_new ? 1 : 0); /* isNewData */
    vm_write64(addr + 8,    stats ? (u64)stats->atime : 0);
    vm_write64(addr + 16,   stats ? (u64)stats->mtime : 0);
    vm_write64(addr + 24,   stats ? (u64)stats->ctime : 0);
    if (dirName) {
        size_t len = strnlen(dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);
        memcpy(vm_base + addr + 32, dirName, len);
        ((char*)vm_base)[addr + 32 + len] = 0;
    }
    if (meta && meta->valid) {
        memcpy(vm_base + addr + 64, meta->param.title, sizeof(meta->param.title));
        memcpy(vm_base + addr + 192, meta->param.subTitle, sizeof(meta->param.subTitle));
        memcpy(vm_base + addr + 320, meta->param.detail, sizeof(meta->param.detail));
        vm_write32(addr + 1344, meta->param.attribute);
        memcpy(vm_base + addr + 1352, meta->param.listParam, sizeof(meta->param.listParam));
    }
    vm_write32(addr + 1616, 0);              /* bind */
    vm_write32(addr + 1620, stats ? (u32)bytes_to_kb(stats->total_bytes) : 0);
    vm_write32(addr + 1624, stats ? (u32)bytes_to_kb(stats->system_bytes) : 0);
    vm_write32(addr + 1628, stats ? stats->file_count : 0);
    vm_write32(addr + 1632, 0);              /* fileListNum */
    vm_write32(addr + 1636, 0);              /* fileList ptr (NULL — no files) */
}

/* Dispatch funcStat callback via the guest-caller hook.
 * Returns the cbResult.result value the callback wrote, or
 * CELL_SAVEDATA_CBRESULT_ERR_FAILURE if no dispatcher is installed. */
static s32 dispatch_func_stat(uint32_t func_opd, int is_new, const char* dirName,
                              uint32_t userdata)
{
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;

    scratch_reset();
    uint32_t cb_ea  = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_STATGET_SIZE);
    uint32_t set_ea = scratch_alloc(SAVEDATA_STATSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) {
        printf("[cellSaveData] scratch alloc failed\n");
        return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    }

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, userdata);
    marshal_statget_init(get_ea, is_new, dirName, NULL, NULL);
    /* StatSet zero-init by scratch_alloc */

    printf("[cellSaveData] dispatching funcStat OPD=0x%08X (cb=0x%X get=0x%X set=0x%X, isNew=%d)\n",
           func_opd, cb_ea, get_ea, set_ea, is_new);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);

    s32 result = marshal_cbresult_read_result(cb_ea);
    printf("[cellSaveData] funcStat returned cbResult.result=%d\n", result);
    return result;
}

/* Enumerate save directories matching a prefix. Returns count, fills dirList up to max. */
static u32 enumerate_save_dirs(const char* prefix, CellSaveDataDirList* dirList, u32 max)
{
    u32 count = 0;

    if (!dir_exists(s_save_root))
        return 0;

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", s_save_root);
        for (char* p = search; *p; p++) {
            if (*p == '/') *p = '\\';
        }

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (fd.cFileName[0] == '.')
                continue;
            if (!savedata_prefix_match(fd.cFileName, prefix))
                continue;
            {
                char full[1024];
                SaveDataMetadata meta;
                snprintf(full, sizeof(full), "%s\\%s", s_save_root, fd.cFileName);
                if (!read_param_sfo(full, &meta))
                    continue;
                if (count < max && dirList) {
                    memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                    strncpy(dirList[count].dirName, fd.cFileName,
                            CELL_SAVEDATA_DIRNAME_SIZE - 1);
                    memcpy(dirList[count].listParam, meta.param.listParam,
                           CELL_SAVEDATA_SYSP_LPARAM_SIZE);
                }
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(s_save_root);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            /* Check it's a directory */
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", s_save_root, de->d_name);
            if (!dir_exists(full))
                continue;
            if (!savedata_prefix_match(de->d_name, prefix))
                continue;
            {
                SaveDataMetadata meta;
                if (!read_param_sfo(full, &meta))
                    continue;
                if (count < max && dirList) {
                    memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                    strncpy(dirList[count].dirName, de->d_name,
                            CELL_SAVEDATA_DIRNAME_SIZE - 1);
                    memcpy(dirList[count].listParam, meta.param.listParam,
                           CELL_SAVEDATA_SYSP_LPARAM_SIZE);
                }
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* Enumerate only callback-visible files. PARAM.SFO/PARAM.PFD are system files
 * and must contribute to sizes but never to fileNum/fileList. */
#ifdef _WIN32
static s64 filetime_to_unix(const FILETIME* value)
{
    ULARGE_INTEGER ticks;
    ticks.LowPart = value->dwLowDateTime;
    ticks.HighPart = value->dwHighDateTime;
    if (ticks.QuadPart < 116444736000000000ULL)
        return 0;
    return (s64)(ticks.QuadPart / 10000000ULL - 11644473600ULL);
}
#endif

static u32 enumerate_save_files(const char* save_path,
                                 const SaveDataMetadata* meta,
                                 CellSaveDataFileStat* fileList, u32 max,
                                 SaveDataDiskStats* stats)
{
    u32 count = 0;
    if (stats) {
        HOST_STAT_T dir_stat;
        memset(stats, 0, sizeof(*stats));
        if (HOST_STAT(save_path, &dir_stat) == 0) {
            stats->atime = (s64)dir_stat.st_atime;
            stats->mtime = (s64)dir_stat.st_mtime;
            stats->ctime = (s64)dir_stat.st_ctime;
        }
    }

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", save_path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            {
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                if (is_system_file(fd.cFileName))
                    continue;
                if (stats)
                    stats->total_bytes += ((u64)sz.QuadPart + 1023u) & ~(u64)1023u;
                if (count < max && fileList) {
                    memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                    fileList[count].fileType = classify_file(meta, fd.cFileName);
                    strncpy(fileList[count].fileName, fd.cFileName,
                            CELL_SAVEDATA_FILENAME_SIZE - 1);
                    fileList[count].st_size = (u64)sz.QuadPart;
                    fileList[count].st_atime = filetime_to_unix(&fd.ftLastAccessTime);
                    fileList[count].st_mtime = filetime_to_unix(&fd.ftLastWriteTime);
                    fileList[count].st_ctime = filetime_to_unix(&fd.ftCreationTime);
                }
                count++;
            }
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(save_path);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", save_path, de->d_name);
            HOST_STAT_T st;
            if (HOST_STAT(full, &st) != 0)
                continue;
#ifdef _WIN32
            if (st.st_mode & _S_IFDIR)
                continue;
#else
            if (S_ISDIR(st.st_mode))
                continue;
#endif
            if (is_system_file(de->d_name))
                continue;
            if (stats)
                stats->total_bytes += ((u64)st.st_size + 1023u) & ~(u64)1023u;
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = classify_file(meta, de->d_name);
                strncpy(fileList[count].fileName, de->d_name,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                fileList[count].st_size = (u64)st.st_size;
                fileList[count].st_atime = (s64)st.st_atime;
                fileList[count].st_mtime = (s64)st.st_mtime;
                fileList[count].st_ctime = (s64)st.st_ctime;
            }
            count++;
        }
        closedir(dp);
    }
#endif

    if (stats) {
        /* Firmware reports a fixed 35 KiB system-data allocation rather than
         * the host byte size of PARAM.SFO/PFD. */
        stats->system_bytes = 35u * 1024u;
        stats->total_bytes += stats->system_bytes;
        stats->file_count = count;
    }
    return count;
}

/* Execute file callback: read/write/delete files in the save directory */
static int savedata_prefix_match(const char* name, const char* prefix)
{
    if (!prefix || !prefix[0])
        return 1;
    const char* p = prefix;
    while (*p) {
        const char* bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len && strncmp(name, p, len) == 0)
            return 1;
        if (!bar) break;
        p = bar + 1;
    }
    return 0;
}

static s32 process_file_op(const char* save_path, CellSaveDataFileSet* set)
{
    if (!set || !is_safe_component(set->fileName, CELL_SAVEDATA_FILENAME_SIZE - 1))
        return -1;

    char file_path[1024];
    snprintf(file_path, sizeof(file_path), "%s/%s", save_path, set->fileName);
#ifdef _WIN32
    for (char* p = file_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    switch (set->fileOperation) {
    case CELL_SAVEDATA_FILEOP_READ: {
        FILE* fp = fopen(file_path, "rb");
        if (!fp) {
            printf("[cellSaveData] file read: cannot open '%s'\n", file_path);
            /* Save_Data-Overview p.21 / Reference p.105 (0x8002920a): a data
             * file the game asked to READ but which does not exist terminates
             * the utility with CELL_SAVEDATA_ERROR_FAILURE. Reporting a
             * successful 0-byte transfer here let the game proceed on an
             * uninitialized buffer (2026-08-04 doc-conformance audit). -2 is
             * the caller's file-not-found sentinel. */
            return -2;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t read_size = (size_t)set->fileSize;
        if (read_size > set->fileBufSize) read_size = set->fileBufSize;
        size_t got = fread(set->fileBuf, 1, read_size, fp);
        fclose(fp);
        return (s32)got;
    }

    case CELL_SAVEDATA_FILEOP_WRITE:
    case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: {
        ensure_dirs(save_path);
        const char* mode;
        if (set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE && set->fileOffset == 0) {
            mode = "wb";
        } else {
            mode = "r+b";
        }
        FILE* fp = fopen(file_path, mode);
        if (!fp) {
            fp = fopen(file_path, "wb");
        }
        if (!fp) {
            printf("[cellSaveData] file write: cannot open '%s'\n", file_path);
            return 0;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t write_size = (size_t)set->fileSize;
        if (write_size > set->fileBufSize) write_size = set->fileBufSize;
        size_t wrote = fwrite(set->fileBuf, 1, write_size, fp);
        if (set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE) {
            u64 final_size = (u64)set->fileOffset + (u64)wrote;
#ifdef _WIN32
            if (_chsize_s(_fileno(fp), final_size) != 0)
                wrote = 0;
#else
            if (ftruncate(fileno(fp), (off_t)final_size) != 0)
                wrote = 0;
#endif
        }
        fclose(fp);
        return (s32)wrote;
    }

    case CELL_SAVEDATA_FILEOP_DELETE:
        remove(file_path);
        return 0;

    default:
        return 0;
    }
}

typedef struct PsfWriteEntry {
    const char* key;
    u16 format;
    const unsigned char* data;
    u32 length;
    u32 maximum;
    u32 integer;
} PsfWriteEntry;

static u32 bounded_string_length(const char* value, u32 maximum)
{
    size_t limit = maximum ? maximum - 1 : 0;
    size_t length = strnlen(value ? value : "", limit);
    return (u32)length + 1;
}

static int write_binary_param_sfo(const char* save_path, const char* dir_name,
                                  const SaveDataMetadata* meta)
{
    PsfWriteEntry entries[11 + SAVEDATA_MAX_SECURE_FILES];
    char secure_keys[SAVEDATA_MAX_SECURE_FILES][CELL_SAVEDATA_FILENAME_SIZE + 1];
    unsigned char zero_account[16] = {0};
    u32 count = 0;
    u32 key_bytes = 0;
    u32 data_bytes = 0;
    u32 key_table;
    u32 data_table;
    size_t total;
    unsigned char* output;
    char path[1024];
    char temp_path[1024];
    FILE* fp;

#define ADD_ARRAY(k, p, n) do { \
    entries[count++] = (PsfWriteEntry){(k), 0x0004, (const unsigned char*)(p), (n), (n), 0}; \
} while (0)
#define ADD_STRING(k, p, n) do { \
    u32 _len = bounded_string_length((p), (n)); \
    entries[count++] = (PsfWriteEntry){(k), 0x0204, (const unsigned char*)(p), _len, (n), 0}; \
} while (0)
#define ADD_INTEGER(k, v) do { \
    entries[count++] = (PsfWriteEntry){(k), 0x0404, NULL, 4, 4, (u32)(v)}; \
} while (0)

    ADD_ARRAY("ACCOUNT_ID", zero_account, sizeof(zero_account));
    ADD_INTEGER("ATTRIBUTE", meta->param.attribute);
    ADD_STRING("CATEGORY", "SD", 4);
    ADD_STRING("DETAIL", meta->param.detail, CELL_SAVEDATA_SYSP_DETAIL_SIZE);
    ADD_ARRAY("PARAMS", NULL, 1024);
    ADD_ARRAY("PARAMS2", NULL, 12);
    ADD_INTEGER("PARENTAL_LEVEL", 0);
    ADD_STRING("SAVEDATA_DIRECTORY", dir_name, CELL_SAVEDATA_DIRNAME_SIZE);
    ADD_STRING("SAVEDATA_LIST_PARAM", meta->param.listParam, CELL_SAVEDATA_SYSP_LPARAM_SIZE);
    ADD_STRING("SUB_TITLE", meta->param.subTitle, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE);
    ADD_STRING("TITLE", meta->param.title, CELL_SAVEDATA_SYSP_TITLE_SIZE);
    for (u32 i = 0; i < meta->secure_file_count; ++i) {
        secure_keys[i][0] = '*';
        strncpy(secure_keys[i] + 1, meta->secure_files[i], CELL_SAVEDATA_FILENAME_SIZE - 1);
        secure_keys[i][CELL_SAVEDATA_FILENAME_SIZE] = '\0';
        ADD_INTEGER(secure_keys[i], 1);
    }
#undef ADD_ARRAY
#undef ADD_STRING
#undef ADD_INTEGER

    for (u32 i = 0; i < count; ++i) {
        size_t key_len = strlen(entries[i].key) + 1;
        if (key_len > UINT16_MAX || key_bytes > UINT16_MAX - key_len)
            return 0;
        key_bytes += (u32)key_len;
        if (entries[i].maximum > UINT32_MAX - data_bytes)
            return 0;
        data_bytes += entries[i].maximum;
    }
    key_table = 20 + count * 16;
    data_table = (key_table + key_bytes + 3u) & ~3u;
    total = (size_t)data_table + data_bytes;
    output = (unsigned char*)calloc(total ? total : 1, 1);
    if (!output)
        return 0;

    memcpy(output, "\0PSF", 4);
    write_le32(output + 4, 0x101);
    write_le32(output + 8, key_table);
    write_le32(output + 12, data_table);
    write_le32(output + 16, count);

    {
        u32 key_off = 0;
        u32 data_off = 0;
        for (u32 i = 0; i < count; ++i) {
            unsigned char* def = output + 20 + i * 16;
            size_t key_len = strlen(entries[i].key) + 1;
            write_le16(def, (u16)key_off);
            write_le16(def + 2, entries[i].format);
            write_le32(def + 4, entries[i].length);
            write_le32(def + 8, entries[i].maximum);
            write_le32(def + 12, data_off);
            memcpy(output + key_table + key_off, entries[i].key, key_len);
            if (entries[i].format == 0x0404)
                write_le32(output + data_table + data_off, entries[i].integer);
            else if (entries[i].data && entries[i].length)
                memcpy(output + data_table + data_off, entries[i].data, entries[i].length);
            key_off += (u32)key_len;
            data_off += entries[i].maximum;
        }
    }

    snprintf(path, sizeof(path), "%s/PARAM.SFO", save_path);
    snprintf(temp_path, sizeof(temp_path), "%s/PARAM.SFO.tmp", save_path);
#ifdef _WIN32
    for (char* p = path; *p; ++p) if (*p == '/') *p = '\\';
    for (char* p = temp_path; *p; ++p) if (*p == '/') *p = '\\';
#endif
    fp = fopen(temp_path, "wb");
    if (!fp || fwrite(output, 1, total, fp) != total || fflush(fp) != 0) {
        if (fp) fclose(fp);
        remove(temp_path);
        free(output);
        return 0;
    }
    fclose(fp);
    free(output);
#ifdef _WIN32
    if (!MoveFileExA(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(temp_path);
        return 0;
    }
#else
    if (rename(temp_path, path) != 0) {
        remove(temp_path);
        return 0;
    }
#endif
    return 1;
}

static uint32_t guest_ea_from_host(const void* ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)vm_base;
    if (value >= base && value - base <= UINT32_MAX)
        return (uint32_t)(value - base);
    return (uint32_t)value;
}

static const char* guest_cstr(uint32_t ea)
{
    return ea ? (const char*)(vm_base + ea) : NULL;
}

static s32 callback_error_to_api(s32 result)
{
    (void)result;
    return CELL_SAVEDATA_ERROR_CBRESULT;
}

static s32 process_file_op_guest(const char* save_path, uint32_t set_ea,
                                 SaveDataMetadata* meta, int is_save)
{
    CellSaveDataFileSet set;
    const char* content_name = NULL;
    memset(&set, 0, sizeof(set));
    set.fileOperation = vm_read32(set_ea + 0);
    set.fileType = vm_read32(set_ea + 8);
    memcpy(set.secureFileId, vm_base + set_ea + 12, sizeof(set.secureFileId));
    set.fileName = (char*)guest_cstr(vm_read32(set_ea + 28));
    set.fileOffset = vm_read32(set_ea + 32);
    set.fileSize = vm_read32(set_ea + 36);
    set.fileBufSize = vm_read32(set_ea + 40);
    set.fileBuf = vm_read32(set_ea + 44) ? vm_base + vm_read32(set_ea + 44) : NULL;

    switch (set.fileType) {
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0: content_name = "ICON0.PNG"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1: content_name = "ICON1.PAM"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1:  content_name = "PIC1.PNG"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_SND0:  content_name = "SND0.AT3"; break;
    default: break;
    }
    if (content_name)
        set.fileName = (char*)content_name;
    if (!set.fileName || !is_safe_component(set.fileName, CELL_SAVEDATA_FILENAME_SIZE - 1))
        return -1;
    if ((set.fileOperation == CELL_SAVEDATA_FILEOP_READ ||
         set.fileOperation == CELL_SAVEDATA_FILEOP_WRITE ||
         set.fileOperation == CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC) &&
        set.fileSize && !set.fileBuf)
        return -1;
    if (is_save && !content_name) {
        if (set.fileOperation == CELL_SAVEDATA_FILEOP_DELETE)
            metadata_set_secure(meta, set.fileName, 0);
        else if (set.fileOperation == CELL_SAVEDATA_FILEOP_WRITE ||
                 set.fileOperation == CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC)
            metadata_set_secure(meta, set.fileName,
                                set.fileType == CELL_SAVEDATA_FILETYPE_SECUREFILE);
    }
    return process_file_op(save_path, &set);
}

/* Core save/load implementation shared by List/Fixed/Auto variants. Callback
 * addresses and callback-visible structures live in guest memory; none of them
 * are native function pointers or native-endian host structures. */
static s32 savedata_execute(const char* dirName, int is_save,
                             CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             uint32_t userdata_ea)
{
    uint32_t stat_opd = guest_ea_from_host((const void*)funcStat);
    uint32_t file_opd = guest_ea_from_host((const void*)funcFile);
    uint32_t setbuf_ea = guest_ea_from_host(setBuf);
    if (!is_safe_component(dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1) ||
        !stat_opd || !g_ps3_guest_caller)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    SaveDataMetadata metadata;
    SaveDataDiskStats disk_stats;
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_exists(save_path);
    memset(&metadata, 0, sizeof(metadata));
    memset(&disk_stats, 0, sizeof(disk_stats));
    if (!is_new)
        read_param_sfo(save_path, &metadata);
    printf("[cellSaveData] %s dir='%s' (new=%d)\n",
           is_save ? "SAVE" : "LOAD", dirName, is_new);

    uint32_t file_max = setbuf_ea ? vm_read32(setbuf_ea + 4) : 0;
    uint32_t buf_size = setbuf_ea ? vm_read32(setbuf_ea + 32) : 0;
    uint32_t buf_ea = setbuf_ea ? vm_read32(setbuf_ea + 36) : 0;
    if (file_max > 4096) file_max = 4096;
    if (file_max * 56u > buf_size) file_max = buf_size / 56u;

    CellSaveDataFileStat* host_files = NULL;
    uint32_t file_num = 0;
    if (!is_new) {
        host_files = (CellSaveDataFileStat*)calloc(file_max, sizeof(*host_files));
        file_num = enumerate_save_files(save_path, &metadata, host_files,
                                        host_files ? file_max : 0, &disk_stats);
    }
    uint32_t listed = file_num < file_max ? file_num : file_max;
    for (uint32_t i = 0; i < listed && buf_ea; ++i) {
        uint32_t dst = buf_ea + i * 56u;
        memset(vm_base + dst, 0, 56u);
        vm_write32(dst + 0, host_files[i].fileType);
        vm_write64(dst + 8, host_files[i].st_size);
        vm_write64(dst + 16, (uint64_t)host_files[i].st_atime);
        vm_write64(dst + 24, (uint64_t)host_files[i].st_mtime);
        vm_write64(dst + 32, (uint64_t)host_files[i].st_ctime);
        memcpy(vm_base + dst + 40, host_files[i].fileName, CELL_SAVEDATA_FILENAME_SIZE);
    }
    free(host_files);

    scratch_reset();
    uint32_t cb_ea = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_STATGET_SIZE);
    uint32_t set_ea = scratch_alloc(SAVEDATA_STATSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea)
        return CELL_SAVEDATA_ERROR_FAILURE;

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, userdata_ea);
    marshal_statget_init(get_ea, is_new, dirName, &metadata, &disk_stats);
    vm_write32(get_ea + 1632, listed);
    vm_write32(get_ea + 1636, listed ? buf_ea : 0);

    g_ps3_guest_caller(stat_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);
    s32 result = marshal_cbresult_read_result(cb_ea);
    userdata_ea = vm_read32(cb_ea + 16);
    printf("[cellSaveData] funcStat returned cbResult.result=%d userdata=0x%08X\n",
           result, userdata_ea);
    if (result < 0) return callback_error_to_api(result);
    if (result != CELL_SAVEDATA_CBRESULT_OK_NEXT) return CELL_OK;

    uint32_t param_ea = vm_read32(set_ea + 0);
    if (is_save && param_ea) {
        memcpy(metadata.param.title, vm_base + param_ea, sizeof(metadata.param.title));
        memcpy(metadata.param.subTitle, vm_base + param_ea + 128,
               sizeof(metadata.param.subTitle));
        memcpy(metadata.param.detail, vm_base + param_ea + 256,
               sizeof(metadata.param.detail));
        metadata.param.attribute = vm_read32(param_ea + 1280);
        memcpy(metadata.param.listParam, vm_base + param_ea + 1288,
               sizeof(metadata.param.listParam));
        metadata.param.title[sizeof(metadata.param.title) - 1] = '\0';
        metadata.param.subTitle[sizeof(metadata.param.subTitle) - 1] = '\0';
        metadata.param.detail[sizeof(metadata.param.detail) - 1] = '\0';
        metadata.param.listParam[sizeof(metadata.param.listParam) - 1] = '\0';
        metadata.valid = 1;
        ensure_dirs(save_path);
    }

    if (file_opd) {
        uint32_t file_get_ea = scratch_alloc(SAVEDATA_FILEGET_SIZE);
        uint32_t file_set_ea = scratch_alloc(SAVEDATA_FILESET_SIZE);
        if (!file_get_ea || !file_set_ea) return CELL_SAVEDATA_ERROR_FAILURE;
        s32 exc_size = 0;
        for (uint32_t iteration = 0; iteration < 4096; ++iteration) {
            memset(vm_base + file_get_ea, 0, SAVEDATA_FILEGET_SIZE);
            memset(vm_base + file_set_ea, 0, SAVEDATA_FILESET_SIZE);
            vm_write32(file_get_ea, (uint32_t)exc_size);
            marshal_cbresult_reset(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT);
            g_ps3_guest_caller(file_opd, cb_ea, file_get_ea, file_set_ea, 0, 0, 0, 0, 0);
            result = marshal_cbresult_read_result(cb_ea);
            userdata_ea = vm_read32(cb_ea + 16);
            if (result != CELL_SAVEDATA_CBRESULT_OK_NEXT) break;
            exc_size = process_file_op_guest(save_path, file_set_ea, &metadata, is_save);
            if (exc_size == -2)
                return CELL_SAVEDATA_ERROR_FAILURE;   /* READ of a missing file */
            if (exc_size < 0)
                return CELL_SAVEDATA_ERROR_PARAM;
        }
        if (result < 0) return callback_error_to_api(result);
    }

    if (is_save && metadata.valid &&
        !write_binary_param_sfo(save_path, dirName, &metadata))
        return CELL_SAVEDATA_ERROR_ACCESS_ERROR;

    printf("[cellSaveData] %s complete for '%s'\n",
           is_save ? "SAVE" : "LOAD", dirName);
    return CELL_OK;
}

static s32 savedata_select_guest(CellSaveDataSetList* setList,
                                  CellSaveDataSetBuf* setBuf,
                                  uint32_t callback_opd, int fixed,
                                  uint32_t* userdata_ea,
                                  char selected[CELL_SAVEDATA_DIRNAME_SIZE],
                                  int* callback_finished)
{
    uint32_t list_ea = guest_ea_from_host(setList);
    uint32_t bufdesc_ea = guest_ea_from_host(setBuf);
    if (!list_ea || !bufdesc_ea || !callback_opd || !g_ps3_guest_caller)
        return CELL_SAVEDATA_ERROR_PARAM;

    uint32_t prefix_ea = vm_read32(list_ea + 8);
    const char* prefix = guest_cstr(prefix_ea);
    uint32_t dir_max = vm_read32(bufdesc_ea + 0);
    uint32_t buf_size = vm_read32(bufdesc_ea + 32);
    uint32_t buf_ea = vm_read32(bufdesc_ea + 36);
    if (dir_max > 4096) dir_max = 4096;
    if (dir_max * sizeof(CellSaveDataDirList) > buf_size)
        dir_max = buf_size / (uint32_t)sizeof(CellSaveDataDirList);

    scratch_reset();
    uint32_t cb_ea = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_LISTGET_SIZE);
    uint32_t set_ea = scratch_alloc(fixed ? SAVEDATA_FIXEDSET_SIZE : SAVEDATA_LISTSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea)
        return CELL_SAVEDATA_ERROR_FAILURE;

    if (!buf_ea && dir_max) {
        buf_ea = scratch_alloc(dir_max * (uint32_t)sizeof(CellSaveDataDirList));
        if (!buf_ea) return CELL_SAVEDATA_ERROR_FAILURE;
    }

    uint32_t dir_num = enumerate_save_dirs(prefix ? prefix : "",
        buf_ea ? (CellSaveDataDirList*)(vm_base + buf_ea) : NULL, dir_max);
    uint32_t listed = dir_num < dir_max ? dir_num : dir_max;
    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, *userdata_ea);
    vm_write32(get_ea + 0, dir_num);
    vm_write32(get_ea + 4, listed);
    vm_write32(get_ea + 8, listed ? buf_ea : 0);
    g_ps3_guest_caller(callback_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);

    s32 result = marshal_cbresult_read_result(cb_ea);
    *userdata_ea = vm_read32(cb_ea + 16);
    if (result < 0) return callback_error_to_api(result);
    if (result != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        *callback_finished = 1;
        return CELL_OK;
    }

    uint32_t name_ea = 0;
    if (fixed) {
        name_ea = vm_read32(set_ea + 0);
    } else {
        uint32_t fixed_num = vm_read32(set_ea + 8);
        uint32_t fixed_ea = vm_read32(set_ea + 12);
        uint32_t focus_ea = vm_read32(set_ea + 4);
        uint32_t new_data_ea = vm_read32(set_ea + 16);
        if (fixed_num && fixed_ea) name_ea = fixed_ea;
        else if (focus_ea) name_ea = focus_ea;
        else if (new_data_ea) name_ea = vm_read32(new_data_ea + 4);
        else if (listed) name_ea = buf_ea;
    }

    selected[0] = '\0';
    if (name_ea) {
        strncpy(selected, guest_cstr(name_ea), CELL_SAVEDATA_DIRNAME_SIZE - 1);
        selected[CELL_SAVEDATA_DIRNAME_SIZE - 1] = '\0';
    }
    return selected[0] ? CELL_OK : CELL_SAVEDATA_ERROR_NODATA;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSaveDataListSave2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListSave2(version=%u)\n", version);
    char selected[CELL_SAVEDATA_DIRNAME_SIZE] = {0};
    int finished = 0;
    uint32_t userdata_ea = guest_ea_from_host(userdata);
    s32 rc = savedata_select_guest(setList, setBuf,
        guest_ea_from_host((const void*)funcList), 0,
        &userdata_ea, selected, &finished);
    if (rc != CELL_OK || finished) return rc;
    return savedata_execute(selected, 1, setBuf, funcStat, funcFile, userdata_ea);
}

s32 cellSaveDataListLoad2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListLoad2(version=%u)\n", version);
    char selected[CELL_SAVEDATA_DIRNAME_SIZE] = {0};
    int finished = 0;
    uint32_t userdata_ea = guest_ea_from_host(userdata);
    s32 rc = savedata_select_guest(setList, setBuf,
        guest_ea_from_host((const void*)funcList), 0,
        &userdata_ea, selected, &finished);
    if (rc != CELL_OK || finished) return rc;
    return savedata_execute(selected, 0, setBuf, funcStat, funcFile, userdata_ea);
}

s32 cellSaveDataFixedSave2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    printf("[cellSaveData] FixedSave2(version=%u)\n", version);
    char selected[CELL_SAVEDATA_DIRNAME_SIZE] = {0};
    int finished = 0;
    uint32_t userdata_ea = guest_ea_from_host(userdata);
    s32 rc = savedata_select_guest(setList, setBuf,
        guest_ea_from_host((const void*)funcFixed), 1,
        &userdata_ea, selected, &finished);
    if (rc != CELL_OK || finished) return rc;
    return savedata_execute(selected, 1, setBuf, funcStat, funcFile, userdata_ea);
}

s32 cellSaveDataFixedLoad2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    printf("[cellSaveData] FixedLoad2(version=%u)\n", version);
    char selected[CELL_SAVEDATA_DIRNAME_SIZE] = {0};
    int finished = 0;
    uint32_t userdata_ea = guest_ea_from_host(userdata);
    s32 rc = savedata_select_guest(setList, setBuf,
        guest_ea_from_host((const void*)funcFixed), 1,
        &userdata_ea, selected, &finished);
    if (rc != CELL_OK || finished) return rc;
    return savedata_execute(selected, 0, setBuf, funcStat, funcFile, userdata_ea);
}

s32 cellSaveDataAutoSave2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] AutoSave2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    return savedata_execute(dirName, 1, setBuf, funcStat, funcFile,
                            guest_ea_from_host(userdata));
}

s32 cellSaveDataAutoLoad2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] AutoLoad2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;
    (void)errDialog; (void)container;
    return savedata_execute(dirName, 0, setBuf, funcStat, funcFile,
                            guest_ea_from_host(userdata));
}

s32 cellSaveDataListAutoSave(u32 version, u32 errDialog,
                              CellSaveDataSetList* setList,
                              CellSaveDataSetBuf* setBuf,
                              CellSaveDataFixedCallback funcFixed,
                              CellSaveDataStatCallback funcStat,
                              CellSaveDataFileCallback funcFile,
                              u32 container, void* userdata)
{
    char selected[CELL_SAVEDATA_DIRNAME_SIZE] = {0};
    int finished = 0;
    uint32_t userdata_ea = guest_ea_from_host(userdata);
    s32 rc;
    printf("[cellSaveData] ListAutoSave(version=%u)\n", version);
    (void)errDialog; (void)container;
    rc = savedata_select_guest(setList, setBuf,
        guest_ea_from_host((const void*)funcFixed), 1,
        &userdata_ea, selected, &finished);
    if (rc != CELL_OK || finished) return rc;
    return savedata_execute(selected, 1, setBuf, funcStat, funcFile, userdata_ea);
}

s32 cellSaveDataListAutoLoad(u32 version, u32 errDialog,
                              CellSaveDataSetList* setList,
                              CellSaveDataSetBuf* setBuf,
                              CellSaveDataFixedCallback funcFixed,
                              CellSaveDataStatCallback funcStat,
                              CellSaveDataFileCallback funcFile,
                              u32 container, void* userdata)
{
    char selected[CELL_SAVEDATA_DIRNAME_SIZE] = {0};
    int finished = 0;
    uint32_t userdata_ea = guest_ea_from_host(userdata);
    s32 rc;
    printf("[cellSaveData] ListAutoLoad(version=%u)\n", version);
    (void)errDialog; (void)container;
    rc = savedata_select_guest(setList, setBuf,
        guest_ea_from_host((const void*)funcFixed), 1,
        &userdata_ea, selected, &finished);
    if (rc != CELL_OK || finished) return rc;
    return savedata_execute(selected, 0, setBuf, funcStat, funcFile, userdata_ea);
}

s32 cellSaveDataDelete2(u32 container)
{
    printf("[cellSaveData] Delete2(container=%u)\n", container);

    /* Without a directory name we can't delete anything meaningful.
       This variant typically shows a UI for deletion - just succeed. */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Old / non-_2 variants — same wrapper pattern as the _2 versions, with
 * the same guest-callback caveat.
 *
 * Older PS3 SDK builds (e.g. our flOw NPUA80001 dump) link the original
 * cellSaveDataAutoSave / AutoLoad / Delete instead of the _2 variants
 * RPCS3's flOw build uses. Same semantics, different NID.
 * -----------------------------------------------------------------------*/
s32 cellSaveDataAutoSave(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    printf("[cellSaveData] AutoSave(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;
    (void)errDialog; (void)container;
    return savedata_execute(dirName, 1, setBuf, funcStat, funcFile,
                            guest_ea_from_host(userdata));
}

s32 cellSaveDataAutoLoad(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    printf("[cellSaveData] AutoLoad(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;
    (void)errDialog; (void)container;
    return savedata_execute(dirName, 0, setBuf, funcStat, funcFile,
                            guest_ea_from_host(userdata));
}

s32 cellSaveDataDelete(u32 version, const char* dirName,
                        u32 container)
{
    (void)version;
    printf("[cellSaveData] Delete(dir='%s', container=%u)\n",
           dirName ? dirName : "<null>", container);
    return CELL_OK;
}
