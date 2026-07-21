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

static void marshal_statget_init(uint32_t addr, int is_new, const char* dirName,
                                  s32 sizeKB, u32 fileNum)
{
    vm_write32(addr + 0,    1024 * 1024);    /* hddFreeSizeKB = 1 GB */
    vm_write32(addr + 4,    is_new ? 1 : 0); /* isNewData */
    vm_write64(addr + 8,    0);              /* dir.st_atime */
    vm_write64(addr + 16,   0);              /* dir.st_mtime */
    vm_write64(addr + 24,   0);              /* dir.st_ctime */
    if (dirName) {
        size_t len = strnlen(dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);
        memcpy(vm_base + addr + 32, dirName, len);
        ((char*)vm_base)[addr + 32 + len] = 0;
    }
    /* getParam (+64..+1615) left zero — no PARAM.SFO data */
    vm_write32(addr + 1616, 0);              /* bind */
    vm_write32(addr + 1620, (uint32_t)sizeKB); /* sizeKB */
    vm_write32(addr + 1624, 0);              /* sysSizeKB */
    vm_write32(addr + 1628, fileNum);        /* fileNum */
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
    marshal_statget_init(get_ea, is_new, dirName, 0, 0);
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

    ensure_dirs(s_save_root);

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
            if (prefix && prefix[0] && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, fd.cFileName,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
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
            if (prefix && prefix[0] && strncmp(de->d_name, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, de->d_name,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* Enumerate files in a save directory. Returns count, fills fileList up to max. */
static u32 enumerate_save_files(const char* save_path,
                                 CellSaveDataFileStat* fileList, u32 max)
{
    u32 count = 0;

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
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = CELL_SAVEDATA_FILETYPE_NORMALFILE;
                strncpy(fileList[count].fileName, fd.cFileName,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                fileList[count].st_size = (u64)sz.QuadPart;
            }
            count++;
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
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = CELL_SAVEDATA_FILETYPE_NORMALFILE;
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

    return count;
}

/* Execute file callback: read/write/delete files in the save directory */
static s32 process_file_op(const char* save_path, CellSaveDataFileSet* set)
{
    if (!set || !set->fileName)
        return CELL_OK;

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
            return 0; /* excSize = 0 */
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

/* Write a simplified PARAM.SFO with the save's title/subtitle/detail */
static void write_param_sfo(const char* save_path, const CellSaveDataSystemFileParam* param)
{
    if (!param) return;

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "wb");
    if (!fp) return;

    /* Write a simplified text-based PARAM.SFO for easy debugging.
       Games don't read this directly - our stat callback fills it from here. */
    fprintf(fp, "TITLE=%s\n", param->title);
    fprintf(fp, "SUB_TITLE=%s\n", param->subTitle);
    fprintf(fp, "DETAIL=%s\n", param->detail);
    fprintf(fp, "ATTRIBUTE=%u\n", param->attribute);
    fprintf(fp, "LIST_PARAM=%s\n", param->listParam);
    fclose(fp);
}

/* Read simplified PARAM.SFO */
static void read_param_sfo(const char* save_path, CellSaveDataSystemFileParam* param)
{
    if (!param) return;
    memset(param, 0, sizeof(CellSaveDataSystemFileParam));

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strncmp(line, "TITLE=", 6) == 0)
            strncpy(param->title, line + 6, CELL_SAVEDATA_SYSP_TITLE_SIZE - 1);
        else if (strncmp(line, "SUB_TITLE=", 10) == 0)
            strncpy(param->subTitle, line + 10, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE - 1);
        else if (strncmp(line, "DETAIL=", 7) == 0)
            strncpy(param->detail, line + 7, CELL_SAVEDATA_SYSP_DETAIL_SIZE - 1);
        else if (strncmp(line, "ATTRIBUTE=", 10) == 0)
            param->attribute = (u32)atoi(line + 10);
        else if (strncmp(line, "LIST_PARAM=", 11) == 0)
            strncpy(param->listParam, line + 11, CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1);
    }
    fclose(fp);
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
    if (result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
        return CELL_SAVEDATA_ERROR_NODATA;
    return CELL_SAVEDATA_ERROR_CBRESULT;
}

static s32 process_file_op_guest(const char* save_path, uint32_t set_ea)
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
    if (!set.fileName)
        return -1;
    if ((set.fileOperation == CELL_SAVEDATA_FILEOP_READ ||
         set.fileOperation == CELL_SAVEDATA_FILEOP_WRITE ||
         set.fileOperation == CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC) &&
        set.fileSize && (!set.fileBuf || set.fileBufSize < set.fileSize))
        return -1;
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
    if (!dirName || !stat_opd || !g_ps3_guest_caller)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_exists(save_path);
    printf("[cellSaveData] %s dir='%s' (new=%d)\n",
           is_save ? "SAVE" : "LOAD", dirName, is_new);

    uint32_t file_max = setbuf_ea ? vm_read32(setbuf_ea + 4) : 0;
    uint32_t buf_size = setbuf_ea ? vm_read32(setbuf_ea + 32) : 0;
    uint32_t buf_ea = setbuf_ea ? vm_read32(setbuf_ea + 36) : 0;
    if (file_max > 4096) file_max = 4096;
    if (file_max * 56u > buf_size) file_max = buf_size / 56u;

    CellSaveDataFileStat* host_files = NULL;
    uint32_t file_num = 0;
    if (!is_new && file_max) {
        host_files = (CellSaveDataFileStat*)calloc(file_max, sizeof(*host_files));
        if (host_files) file_num = enumerate_save_files(save_path, host_files, file_max);
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
    marshal_statget_init(get_ea, is_new, dirName, 0, file_num);
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
        CellSaveDataSystemFileParam param;
        memset(&param, 0, sizeof(param));
        memcpy(param.title, vm_base + param_ea, sizeof(param.title));
        memcpy(param.subTitle, vm_base + param_ea + 128, sizeof(param.subTitle));
        memcpy(param.detail, vm_base + param_ea + 256, sizeof(param.detail));
        param.attribute = vm_read32(param_ea + 1280);
        memcpy(param.listParam, vm_base + param_ea + 1288, sizeof(param.listParam));
        ensure_dirs(save_path);
        write_param_sfo(save_path, &param);
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
            exc_size = process_file_op_guest(save_path, file_set_ea);
            if (exc_size < 0)
                return CELL_SAVEDATA_ERROR_PARAM;
        }
        if (result < 0) return callback_error_to_api(result);
    }

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
    (void)version; (void)errDialog; (void)setBuf; (void)funcFile;
    (void)container; (void)userdata;
    printf("[cellSaveData] AutoLoad2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* funcStat is the GAME's guest-OPD address. Marshal StatGet/Set/CB
     * into guest BE memory and dispatch through g_ps3_guest_caller.
     * Even on first-run-no-data, the game observes funcStat fire with
     * isNewData=1 so its title state machine can advance. */
    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_exists(save_path);

    uint32_t func_opd = (uint32_t)(uintptr_t)funcStat;
    s32 cb = dispatch_func_stat(func_opd, is_new, dirName,
                                guest_ea_from_host(userdata));

    if (cb < 0) {
        if (cb == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }
    /* OK_LAST or OK_NEXT — with no actual file load infrastructure for
     * now, succeed without invoking funcFile. */
    return CELL_OK;
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
    /* No guest-callback marshalling yet — succeed without running funcStat. */
    return CELL_OK;
}

s32 cellSaveDataAutoLoad(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    (void)version; (void)errDialog; (void)setBuf; (void)funcFile;
    (void)container; (void)userdata;
    printf("[cellSaveData] AutoLoad(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_exists(save_path);

    uint32_t func_opd = (uint32_t)(uintptr_t)funcStat;
    s32 cb = dispatch_func_stat(func_opd, is_new, dirName,
                                guest_ea_from_host(userdata));

    if (cb < 0) {
        if (cb == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }
    return CELL_OK;
}

s32 cellSaveDataDelete(u32 version, const char* dirName,
                        u32 container)
{
    (void)version;
    printf("[cellSaveData] Delete(dir='%s', container=%u)\n",
           dirName ? dirName : "<null>", container);
    return CELL_OK;
}
