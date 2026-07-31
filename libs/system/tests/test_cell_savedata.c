#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ps3emu/guest_call.h"

uint8_t* vm_base = NULL;
ps3_guest_caller_fn g_ps3_guest_caller = NULL;
int g_yz_watch_dlist = 0;
unsigned long g_yz_dlist_w = 0;
int g_yz_slotstore = 0;
volatile long g_yz_a010_release_scene_active = 0;
volatile long g_yz_a010_root_active = 0;
int g_yz_a010_ppucmd = 0;
void yz_slotstore_log(uint32_t addr, unsigned long long value, int width, void* ra)
{
    (void)addr; (void)value; (void)width; (void)ra;
}
void yz_a010_reltrace_ppu_store(uint32_t addr, uint32_t value, uint32_t guest_pc)
{
    (void)addr; (void)value; (void)guest_pc;
}
void yz_a010_ppucmd_log(uint32_t addr, uint32_t value, void* ra, int width)
{
    (void)addr; (void)value; (void)ra; (void)width;
}

/* Keep the format/parser/enumerator tests coupled to the implementation's
 * private helpers without exporting test-only API from the runtime. */
#include "../cellSaveData.c"

/* CMake's default test configuration can define NDEBUG.  Keep checks active,
 * and, importantly, keep expressions with fixture-setup side effects active. */
#undef assert
#define assert(condition)                                                       \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s (%s:%d)\n",                       \
                    #condition, __FILE__, __LINE__);                            \
            abort();                                                            \
        }                                                                       \
    } while (0)

static void write_bytes(const char* directory, const char* name, size_t size)
{
    char path[1024];
    FILE* fp;
    unsigned char byte = 0x5a;
    snprintf(path, sizeof(path), "%s\\%s", directory, name);
    fp = fopen(path, "wb");
    assert(fp);
    for (size_t i = 0; i < size; ++i)
        assert(fwrite(&byte, 1, 1, fp) == 1);
    fclose(fp);
}

static u32 find_type(const CellSaveDataFileStat* files, u32 count, const char* name)
{
    for (u32 i = 0; i < count; ++i) {
        if (_stricmp(files[i].fileName, name) == 0)
            return files[i].fileType;
    }
    return UINT32_MAX;
}

static const char* g_expected_user_file;
static u32 g_expected_user_size;
static u32 g_file_callback_count;

static void fake_guest_callback(uint32_t opd_addr,
                                uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                uint64_t arg3, uint64_t arg4, uint64_t arg5,
                                uint64_t arg6, uint64_t arg7)
{
    (void)arg3; (void)arg4; (void)arg5; (void)arg6; (void)arg7;
    if (opd_addr == 0x100) {
        u32 file_list = vm_read32((u32)arg1 + 1636);
        assert(vm_read32((u32)arg1 + 4) == 0);
        assert(vm_read32((u32)arg1 + 1628) == 3);
        assert(vm_read32((u32)arg1 + 1632) == 3);
        assert(file_list != 0);
        assert(((char*)vm_base + (u32)arg1 + 64)[0] != '\0');
        /* The guest list is big-endian for scalar fields, unlike the host
         * helper array used by find_type(). */
        {
            int saw_user = 0;
            for (u32 i = 0; i < 3; ++i) {
                u32 item = file_list + i * 56;
                const char* name = (const char*)vm_base + item + 40;
                if (_stricmp(name, g_expected_user_file) == 0) {
                    assert(vm_read32(item) == CELL_SAVEDATA_FILETYPE_SECUREFILE);
                    assert(vm_read64(item + 8) == g_expected_user_size);
                    saw_user = 1;
                }
            }
            assert(saw_user);
        }
        vm_write32((u32)arg0, CELL_SAVEDATA_CBRESULT_OK_NEXT);
        return;
    }
    assert(opd_addr == 0x200);
    if (g_file_callback_count++ == 0) {
        assert(vm_read32((u32)arg1) == 0);
        strcpy((char*)vm_base + 0x3000, g_expected_user_file);
        vm_write32((u32)arg2 + 0, CELL_SAVEDATA_FILEOP_READ);
        vm_write32((u32)arg2 + 8, CELL_SAVEDATA_FILETYPE_SECUREFILE);
        vm_write32((u32)arg2 + 28, 0x3000);
        vm_write32((u32)arg2 + 32, 0);
        vm_write32((u32)arg2 + 36, g_expected_user_size);
        vm_write32((u32)arg2 + 40, g_expected_user_size);
        vm_write32((u32)arg2 + 44, 0x4000);
        vm_write32((u32)arg0, CELL_SAVEDATA_CBRESULT_OK_NEXT);
    } else {
        assert(vm_read32((u32)arg1) == g_expected_user_size);
        vm_write32((u32)arg0, CELL_SAVEDATA_CBRESULT_OK_LAST);
    }
}

static void verify_callback_load(const char* directory, const char* expected_user_file)
{
    char parent[MAX_PATH];
    char user_path[MAX_PATH];
    char* slash;
    HOST_STAT_T st;
    const u32 setbuf_ea = 0x1000;
    const u32 list_ea = 0x2000;
    s32 result;

    strncpy(parent, directory, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    slash = strrchr(parent, '\\');
    if (!slash) slash = strrchr(parent, '/');
    assert(slash);
    *slash++ = '\0';
    assert(is_safe_component(slash, CELL_SAVEDATA_DIRNAME_SIZE - 1));
    strncpy(s_save_root, parent, sizeof(s_save_root) - 1);
    s_save_root[sizeof(s_save_root) - 1] = '\0';

    snprintf(user_path, sizeof(user_path), "%s\\%s", directory, expected_user_file);
    assert(HOST_STAT(user_path, &st) == 0);
    assert(st.st_size > 0 && (u64)st.st_size < 0x200000);
    g_expected_user_file = expected_user_file;
    g_expected_user_size = (u32)st.st_size;
    g_file_callback_count = 0;

    vm_base = (uint8_t*)calloc(0x02600000, 1);
    assert(vm_base);
    vm_write32(setbuf_ea + 4, 3);
    vm_write32(setbuf_ea + 32, 3 * 56);
    vm_write32(setbuf_ea + 36, list_ea);
    g_ps3_guest_caller = fake_guest_callback;
    result = savedata_execute(slash, 0,
        (CellSaveDataSetBuf*)(vm_base + setbuf_ea),
        (CellSaveDataStatCallback)(uintptr_t)0x100,
        (CellSaveDataFileCallback)(uintptr_t)0x200, 0);
    assert(result == CELL_OK);
    assert(g_file_callback_count == 2);
    free(vm_base);
    vm_base = NULL;
    g_ps3_guest_caller = NULL;
}

static void verify_save_directory(const char* directory, const char* expected_user_file)
{
    SaveDataMetadata metadata;
    SaveDataDiskStats stats;
    CellSaveDataFileStat files[8];
    u32 count;
    assert(read_param_sfo(directory, &metadata));
    assert(metadata.valid);
    assert(metadata.param.title[0]);
    memset(files, 0, sizeof(files));
    count = enumerate_save_files(directory, &metadata, files, 8, &stats);
    assert(count == 3);
    assert(stats.file_count == 3);
    assert(find_type(files, count, "ICON0.PNG") == CELL_SAVEDATA_FILETYPE_CONTENT_ICON0);
    assert(find_type(files, count, "PIC1.PNG") == CELL_SAVEDATA_FILETYPE_CONTENT_PIC1);
    assert(find_type(files, count, expected_user_file) == CELL_SAVEDATA_FILETYPE_SECUREFILE);
}

int main(int argc, char** argv)
{
    char temp_base[MAX_PATH];
    char temp_file[MAX_PATH];
    SaveDataMetadata written;
    SaveDataMetadata parsed;
    SaveDataDiskStats stats;
    CellSaveDataFileStat files[3];
    CellSaveDataFileSet unsafe_op;
    unsigned char magic[4];
    FILE* fp;

    assert(GetTempPathA(MAX_PATH, temp_base) != 0);
    assert(GetTempFileNameA(temp_base, "sdt", 0, temp_file) != 0);
    assert(DeleteFileA(temp_file));
    assert(CreateDirectoryA(temp_file, NULL));

    memset(&written, 0, sizeof(written));
    strcpy(written.param.title, "SaveData round trip");
    strcpy(written.param.subTitle, "Fixture");
    strcpy(written.param.detail, "Temporary fixture only");
    strcpy(written.param.listParam, "TEST");
    written.param.attribute = CELL_SAVEDATA_ATTR_NODUPLICATE;
    written.valid = 1;
    metadata_set_secure(&written, "USERDATA", 1);
    assert(write_binary_param_sfo(temp_file, "TEST00001", &written));

    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\PARAM.SFO", temp_file);
        fp = fopen(path, "rb");
        assert(fp);
        assert(fread(magic, 1, sizeof(magic), fp) == sizeof(magic));
        fclose(fp);
        assert(memcmp(magic, "\0PSF", 4) == 0);
    }

    assert(read_param_sfo(temp_file, &parsed));
    assert(strcmp(parsed.param.title, written.param.title) == 0);
    assert(strcmp(parsed.param.listParam, written.param.listParam) == 0);
    assert(parsed.param.attribute == written.param.attribute);
    assert(metadata_is_secure(&parsed, "USERDATA"));

    write_bytes(temp_file, "USERDATA", 23);
    write_bytes(temp_file, "ICON0.PNG", 29);
    write_bytes(temp_file, "PIC1.PNG", 31);
    write_bytes(temp_file, "PARAM.PFD", 37);
    memset(files, 0, sizeof(files));
    assert(enumerate_save_files(temp_file, &parsed, files, 3, &stats) == 3);
    assert(stats.file_count == 3);
    assert(stats.system_bytes >= 37);
    assert(find_type(files, 3, "USERDATA") == CELL_SAVEDATA_FILETYPE_SECUREFILE);
    assert(find_type(files, 3, "ICON0.PNG") == CELL_SAVEDATA_FILETYPE_CONTENT_ICON0);
    assert(find_type(files, 3, "PIC1.PNG") == CELL_SAVEDATA_FILETYPE_CONTENT_PIC1);

    memset(&unsafe_op, 0, sizeof(unsafe_op));
    unsafe_op.fileOperation = CELL_SAVEDATA_FILEOP_READ;
    unsafe_op.fileName = "../USERDATA";
    assert(process_file_op(temp_file, &unsafe_op) < 0);

    if (argc > 1) {
        verify_save_directory(argv[1], argc > 2 ? argv[2] : "USERDATA");
        verify_callback_load(argv[1], argc > 2 ? argv[2] : "USERDATA");
    }

    {
        static const char* names[] = {
            "PARAM.SFO", "PARAM.PFD", "USERDATA", "ICON0.PNG", "PIC1.PNG"
        };
        char path[MAX_PATH];
        for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            snprintf(path, sizeof(path), "%s\\%s", temp_file, names[i]);
            DeleteFileA(path);
        }
    }
    assert(RemoveDirectoryA(temp_file));
    puts("cellSaveData tests passed");
    return 0;
}
