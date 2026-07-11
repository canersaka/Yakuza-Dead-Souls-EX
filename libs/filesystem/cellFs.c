/*
 * ps3recomp - cellFs HLE implementation
 *
 * Maps PS3 virtual filesystem paths (/dev_hdd0/, /dev_bdvd/, /app_home/, etc.)
 * to host filesystem paths and performs real I/O.
 */

#include "cellFs.h"
#include "ps3emu/endian.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "adx_decode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p)   _mkdir(p)
#  define HOST_STAT       _stat64
#  define HOST_STAT_T     struct __stat64
#  define HOST_FSTAT      _fstat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p)   mkdir(p, 0755)
#  define HOST_STAT       stat
#  define HOST_STAT_T     struct stat
#  define HOST_FSTAT      fstat
#endif

/* ---------------------------------------------------------------------------
 * FS latency model (env YZ_FS_LAT=<usec>, s29 ledger #67)
 *
 * MEASURED: a pure logging flag (YZ_FS_TRACE, ~usec of printf inside Fstat/
 * Lseek) deterministically flips whether the CRI preloader stages its next
 * step (3/3 flagged boots read scenario.bin, 0/6 unflagged) — the game's own
 * cross-thread staging protocol implicitly depends on FS syscalls taking
 * REALISTIC time (real HDD/BD latency is micro-to-milliseconds; our host
 * page-cache returns in ~100 ns). This is a hardware-timing MODEL (like the
 * vblank), not a force: every call keeps its exact result, it just doesn't
 * return sooner than the floor. QPC busy-wait for sub-ms precision. Applied
 * at the completion boundary (before return) of the data-path entry points.
 * Default OFF until the A/B validates; retirement condition: if the real
 * divergence is later pinned to a specific missing ordering on OUR side,
 * prefer that and retire this.
 * -----------------------------------------------------------------------*/
static void yz_fs_lat_wait(void)
{
#ifdef _WIN32
    static int lat_us = -2;
    if (lat_us == -2) {
        const char* e = getenv("YZ_FS_LAT");
        lat_us = e ? atoi(e) : 0;
        if (lat_us > 0)
            fprintf(stderr, "[fs-lat] ARMED: %d us floor per cellFs data call\n", lat_us);
        else if (lat_us == -1)
            fprintf(stderr, "[fs-lat] ARMED: stderr LOCK-TOUCH mode (s29 rendezvous discriminator)\n");
        else if (lat_us == -3)
            fprintf(stderr, "[fs-lat] ARMED: STDOUT LOCK-TOUCH mode (s30 rendezvous discriminator)\n");
    }
    if (lat_us == 0) return;
    if (lat_us == -1) {
        /* Discriminator (s29): the FS_TRACE flip may be the fprintf's CRT
         * stderr lock acting as an accidental cross-thread rendezvous (a
         * concurrent printer, e.g. t2's signal-path trace, holds it) rather
         * than the print's duration — 200 us of pure busy-wait did NOT
         * reproduce the flip (s29lat). Take+release the lock, emit nothing:
         * duration ~0, ordering edge identical to a print. */
        _lock_file(stderr);
        _unlock_file(stderr);
        return;
    }
    if (lat_us == -3) {
        /* Discriminator (s30 fresh-eyes): -1 tested the WRONG STREAM.
         * YZ_FS_TRACE's per-read/Lseek prints go to STDOUT — the lock t1's
         * frame-loop printf sites (cellGcm etc.) contend on — while the -1
         * mode touched stderr, which t1's producer path doesn't take. Same
         * zero-duration take+release, correct object this time. Reads
         * scenario.bin => the stdout CRT-lock rendezvous IS the flip
         * mechanism; stays unread => volume/flush-yield candidates next. */
        _lock_file(stdout);
        _unlock_file(stdout);
        return;
    }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const LONGLONG ticks = (f.QuadPart * (LONGLONG)lat_us) / 1000000;
    do { QueryPerformanceCounter(&t1); } while (t1.QuadPart - t0.QuadPart < ticks);
#endif
}

/* ---------------------------------------------------------------------------
 * Path translation
 * -----------------------------------------------------------------------*/

#define MAX_PATH_MAPPINGS 16

typedef struct {
    char ps3_prefix[128];
    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    int  in_use;
} PathMapping;

static PathMapping s_path_mappings[MAX_PATH_MAPPINGS];
static char s_root_path[CELL_FS_MAX_FS_PATH_LENGTH] = ".";

static void init_default_mappings(void)
{
    static int s_initialized = 0;
    if (s_initialized)
        return;
    s_initialized = 1;

    cellfs_add_path_mapping("/dev_hdd0/",  "gamedata/dev_hdd0/");
    cellfs_add_path_mapping("/dev_bdvd/",  "gamedata/dev_bdvd/");
    cellfs_add_path_mapping("/dev_flash/", "gamedata/dev_flash/");
    cellfs_add_path_mapping("/app_home/",  "gamedata/app_home/");
    cellfs_add_path_mapping("/dev_usb000/","gamedata/dev_usb/");
}

void cellfs_set_root_path(const char* root)
{
    if (!root) return;
    strncpy(s_root_path, root, sizeof(s_root_path) - 1);
    s_root_path[sizeof(s_root_path) - 1] = '\0';
}

void cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path)
{
    if (!ps3_prefix || !host_path) return;

    /* Try to find existing mapping for this prefix */
    for (int i = 0; i < MAX_PATH_MAPPINGS; i++) {
        if (s_path_mappings[i].in_use &&
            strcmp(s_path_mappings[i].ps3_prefix, ps3_prefix) == 0) {
            strncpy(s_path_mappings[i].host_path, host_path, sizeof(s_path_mappings[i].host_path) - 1);
            s_path_mappings[i].host_path[sizeof(s_path_mappings[i].host_path) - 1] = '\0';
            return;
        }
    }

    /* Add new mapping */
    for (int i = 0; i < MAX_PATH_MAPPINGS; i++) {
        if (!s_path_mappings[i].in_use) {
            s_path_mappings[i].in_use = 1;
            strncpy(s_path_mappings[i].ps3_prefix, ps3_prefix, sizeof(s_path_mappings[i].ps3_prefix) - 1);
            s_path_mappings[i].ps3_prefix[sizeof(s_path_mappings[i].ps3_prefix) - 1] = '\0';
            strncpy(s_path_mappings[i].host_path, host_path, sizeof(s_path_mappings[i].host_path) - 1);
            s_path_mappings[i].host_path[sizeof(s_path_mappings[i].host_path) - 1] = '\0';
            return;
        }
    }
}

/* Translate a PS3 path to a host path. Returns 0 on success, -1 if no mapping found.
 * Also exposed publicly as cellfs_translate_path() for use by other modules. */
static int translate_path(const char* ps3_path, char* host_buf, size_t buf_size)
{
    init_default_mappings();

    if (!ps3_path || !host_buf || buf_size == 0)
        return -1;

    /* Find longest matching prefix */
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < MAX_PATH_MAPPINGS; i++) {
        if (!s_path_mappings[i].in_use)
            continue;
        size_t plen = strlen(s_path_mappings[i].ps3_prefix);
        if (plen > best_len && strncmp(ps3_path, s_path_mappings[i].ps3_prefix, plen) == 0) {
            best = i;
            best_len = plen;
        }
    }

    if (best < 0)
        return -1;

    const char* remainder = ps3_path + best_len;
    snprintf(host_buf, buf_size, "%s/%s%s", s_root_path,
             s_path_mappings[best].host_path, remainder);

    /* Normalize slashes */
    for (char* p = host_buf; *p; p++) {
#ifdef _WIN32
        if (*p == '/') *p = '\\';
#else
        if (*p == '\\') *p = '/';
#endif
    }

    return 0;
}

/* Public wrapper for translate_path, usable by other modules (e.g. codec libs). */
int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size)
{
    return translate_path(ps3_path, host_buf, buf_size);
}

/* ---------------------------------------------------------------------------
 * Host stat -> CellFsStat conversion
 * -----------------------------------------------------------------------*/

/* CellFsStat is written into guest (big-endian) memory; byte-swap every
 * multi-byte field in place. The game reads st_size etc. via lwz/ld which
 * byte-swap, so a host-endian struct yields garbage (e.g. a 0x...00 size that
 * fails asset parsing). Call after filling the struct natively. */
static void cellfs_stat_to_be(CellFsStat* sb)
{
    sb->st_mode    = (s32)ps3_bswap32((u32)sb->st_mode);
    sb->st_uid     = (s32)ps3_bswap32((u32)sb->st_uid);
    sb->st_gid     = (s32)ps3_bswap32((u32)sb->st_gid);
    sb->st_atime   = (s64)ps3_bswap64((u64)sb->st_atime);
    sb->st_mtime   = (s64)ps3_bswap64((u64)sb->st_mtime);
    sb->st_ctime   = (s64)ps3_bswap64((u64)sb->st_ctime);
    sb->st_size    = ps3_bswap64(sb->st_size);
    sb->st_blksize = ps3_bswap64(sb->st_blksize);
}

static void fill_cellfs_stat(CellFsStat* sb, const HOST_STAT_T* hst)
{
    memset(sb, 0, sizeof(CellFsStat));

#ifdef _WIN32
    if (hst->st_mode & _S_IFDIR)
        sb->st_mode = CELL_FS_S_IFDIR;
    else
        sb->st_mode = CELL_FS_S_IFREG;

    if (hst->st_mode & _S_IREAD)
        sb->st_mode |= CELL_FS_S_IRUSR | CELL_FS_S_IRGRP | CELL_FS_S_IROTH;
    if (hst->st_mode & _S_IWRITE)
        sb->st_mode |= CELL_FS_S_IWUSR | CELL_FS_S_IWGRP | CELL_FS_S_IWOTH;
    if (hst->st_mode & _S_IEXEC)
        sb->st_mode |= CELL_FS_S_IXUSR | CELL_FS_S_IXGRP | CELL_FS_S_IXOTH;
#else
    if (S_ISDIR(hst->st_mode))
        sb->st_mode = CELL_FS_S_IFDIR;
    else if (S_ISLNK(hst->st_mode))
        sb->st_mode = CELL_FS_S_IFLNK;
    else
        sb->st_mode = CELL_FS_S_IFREG;

    if (hst->st_mode & S_IRUSR) sb->st_mode |= CELL_FS_S_IRUSR;
    if (hst->st_mode & S_IWUSR) sb->st_mode |= CELL_FS_S_IWUSR;
    if (hst->st_mode & S_IXUSR) sb->st_mode |= CELL_FS_S_IXUSR;
    if (hst->st_mode & S_IRGRP) sb->st_mode |= CELL_FS_S_IRGRP;
    if (hst->st_mode & S_IWGRP) sb->st_mode |= CELL_FS_S_IWGRP;
    if (hst->st_mode & S_IXGRP) sb->st_mode |= CELL_FS_S_IXGRP;
    if (hst->st_mode & S_IROTH) sb->st_mode |= CELL_FS_S_IROTH;
    if (hst->st_mode & S_IWOTH) sb->st_mode |= CELL_FS_S_IWOTH;
    if (hst->st_mode & S_IXOTH) sb->st_mode |= CELL_FS_S_IXOTH;
#endif

    sb->st_uid   = 0;
    sb->st_gid   = 0;
    sb->st_atime = (s64)hst->st_atime;
    sb->st_mtime = (s64)hst->st_mtime;
    sb->st_ctime = (s64)hst->st_ctime;
    sb->st_size  = (u64)hst->st_size;
    sb->st_blksize = 4096;

    cellfs_stat_to_be(sb);
}

/* ---------------------------------------------------------------------------
 * Internal file/dir state
 * -----------------------------------------------------------------------*/

#define MAX_OPEN_FILES 256
#define MAX_OPEN_DIRS  64

typedef struct {
    int    in_use;
    char   path[CELL_FS_MAX_FS_PATH_LENGTH];
    FILE*  host_fp;
    s32    flags;
    /* s30: once-per-open first-read stderr marker — the file-chain walk
     * (scenario.bin -> player_pos.bin -> ... -> all_csb.par) must be readable
     * WITHOUT YZ_FS_TRACE (whose stdout flood is the ledger-#67 treatment
     * itself). stderr is measured NOT the flip mechanism (s29lock inert);
     * volume ~1 line per open, LESSONS #6c bounded. */
    int    read_seen;
} FsFileSlot;

typedef struct {
    int  in_use;
    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
#ifdef _WIN32
    HANDLE           find_handle;
    WIN32_FIND_DATAA find_data;
    int              first_read;
    int              done;
#else
    DIR* host_dir;
#endif
} FsDirSlot;

static FsFileSlot s_files[MAX_OPEN_FILES];
static FsDirSlot  s_dirs[MAX_OPEN_DIRS];

static CellFsFd alloc_fd(void)
{
    for (int i = 3; i < MAX_OPEN_FILES; i++) {  /* skip 0,1,2 = stdin/out/err */
        if (!s_files[i].in_use) {
            s_files[i].in_use = 1;
            s_files[i].read_seen = 0;
            return i;
        }
    }
    return -1;
}

static CellFsDir alloc_dir(void)
{
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!s_dirs[i].in_use) {
            memset(&s_dirs[i], 0, sizeof(FsDirSlot));
            s_dirs[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * s30 dark-trace (fresh-eyes discriminator, ledger #67): YZ_FS_TRACE=2 does
 * the IDENTICAL trace work as mode 1 (same per-read ftell, same formatting)
 * but writes to a private 4KB-buffered file instead of stdout. Splits the
 * flip-flag into its atoms: dark mode flips the boot => the mechanism is the
 * LOCAL WORK (ftell/duration/position) and stdout is exonerated; doesn't
 * flip => the mechanism is the stdout OUTPUT path (lock/volume/flush —
 * YZ_FS_LAT=-3 separates the lock edge next).
 * -----------------------------------------------------------------------*/
static int yz_fs_trace_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("YZ_FS_TRACE");
        int m = e ? atoi(e) : 0;
        if (e && m == 0) m = 1;   /* legacy: any non-numeric value = mode 1 */
        if (m == 2)
            fprintf(stderr, "[fs-trace] ARMED: DARK mode 2 (private-file trace, zero stdout)\n");
        mode = m;
    }
    return mode;
}

static void yz_fs_dark_puts(const char* line)
{
    /* First FS calls happen on one thread long before the CRI fleet spawns,
     * so the lazy-open race is theoretical; a double fopen would only leak a
     * handle, never corrupt (diag-only code). */
    static FILE* darkf = NULL;
    if (!darkf) {
        darkf = fopen("scratch/fs_darktrace.log", "w");
        if (darkf) setvbuf(darkf, NULL, _IOFBF, 4096);
    }
    if (darkf) fputs(line, darkf);
}

/* Build fopen mode string from PS3 flags */
static const char* flags_to_mode(s32 flags)
{
    int access  = flags & CELL_FS_O_ACCMODE;
    int create  = flags & CELL_FS_O_CREAT;
    int trunc   = flags & CELL_FS_O_TRUNC;
    int append  = flags & CELL_FS_O_APPEND;

    if (access == CELL_FS_O_RDONLY) {
        return "rb";
    } else if (access == CELL_FS_O_WRONLY) {
        if (append)       return "ab";
        if (create && trunc) return "wb";
        if (create)       return "wb";
        if (trunc)        return "wb";
        return "r+b";  /* write-only to existing file */
    } else { /* RDWR */
        if (append)       return "a+b";
        if (create && trunc) return "w+b";
        if (create)       return "a+b";  /* create if needed, don't truncate */
        if (trunc)        return "w+b";
        return "r+b";
    }
}

/* Recursively create directories for a path (like mkdir -p) */
static void ensure_parent_dirs(const char* path)
{
    char tmp[CELL_FS_MAX_FS_PATH_LENGTH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Find last separator */
    char* last_sep = NULL;
    for (char* p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\')
            last_sep = p;
    }
    if (!last_sep) return;
    *last_sep = '\0';

    /* Create each component */
    for (char* p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (strlen(tmp) > 0)
                HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    if (strlen(tmp) > 0)
        HOST_MKDIR(tmp);
}

/* ---------------------------------------------------------------------------
 * File operations
 * -----------------------------------------------------------------------*/

/* guest thread id for trace attribution (2026-07-03 late: which thread does
 * which I/O — cri_dlg vs the async workers vs t1) */
extern uint32_t yz_thread_current_id(void);

/* NID: 0x718BF5F8 */
s32 cellFsOpen(const char* path, s32 flags, CellFsFd* fd, const void* arg, u64 size)
{
    printf("[cellFs] t%u Open(path='%s', flags=0x%X)\n",
           yz_thread_current_id(), path ? path : "<null>", flags);

    if (!path || !fd)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0) {
        printf("[cellFs] Open: no path mapping for '%s'\n", path);
        return (s32)CELL_ENOENT;
    }

    /* If creating, ensure parent directories exist */
    if (flags & CELL_FS_O_CREAT) {
        ensure_parent_dirs(host_path);
    }

    const char* mode = flags_to_mode(flags);
    FILE* fp = fopen(host_path, mode);

    /* If open for read failed and CREAT is set, try creating */
    if (!fp && (flags & CELL_FS_O_CREAT)) {
        fp = fopen(host_path, "w+b");
    }

    if (!fp) {
        printf("[cellFs] Open: fopen('%s', '%s') failed: %s\n", host_path, mode, strerror(errno));
        if (errno == ENOENT) return (s32)CELL_ENOENT;
        if (errno == EACCES) return (s32)CELL_EPERM;
        return (s32)CELL_ENOENT;
    }

    CellFsFd slot = alloc_fd();
    if (slot < 0) {
        fclose(fp);
        return CELL_FS_ERROR_EMFILE;
    }

    strncpy(s_files[slot].path, path, CELL_FS_MAX_FS_PATH_LENGTH - 1);
    s_files[slot].path[CELL_FS_MAX_FS_PATH_LENGTH - 1] = '\0';
    s_files[slot].flags   = flags;
    s_files[slot].host_fp = fp;

    *fd = (CellFsFd)ps3_bswap32((u32)slot);   /* guest reads the fd big-endian */
    printf("[cellFs] Open: fd=%d -> '%s'\n", slot, host_path);

    /* CRI-gate diag (pt29, env YZ_CODEC_WATCH): when the intro voice container opens,
     * arm a read-watch on the cri_audio codec-registry entry (guest EA 0x135D9E0 ->
     * SPU image 0x012B4980). Armed HERE (not at boot) to skip the init-time registry
     * enumeration + let the boot reach the streaming phase at full speed. If 0x135D9E0
     * is read AFTER this -> the player reaches codec dispatch (the [watch] dump names
     * the fn); if never -> dispatch is never reached (upstream gate). */
    if (getenv("YZ_CODEC_WATCH") && (strstr(path, "adv_voice") || strstr(path, ".cvm"))) {
        extern void yz_watch_arm_read(u32 guest_addr);
        static int armed = 0;
        if (!armed) { armed = 1; yz_watch_arm_read(0x0135D9E0u); }
    }
    yz_fs_lat_wait();
    return CELL_OK;
}

/* NID: 0x4D5FF8E2 */
s32 cellFsClose(CellFsFd fd)
{
    printf("[cellFs] Close(fd=%d)\n", fd);
    yz_fs_lat_wait();

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (s_files[fd].host_fp) {
        fclose(s_files[fd].host_fp);
        s_files[fd].host_fp = NULL;
    }
    s_files[fd].in_use = 0;

    return CELL_OK;
}

/* NID: 0xBABF9143 */
s32 cellFsRead(CellFsFd fd, void* buf, u64 nbytes, u64* nread)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (!buf)
        return CELL_EFAULT;

    u64 bytes_read = 0;

    /* CRI-gate diag (pt29): trace reads on stream/movie containers (.cvm/.sfd) so
     * we can see whether the criMana player ever reads its header + what bytes
     * come back. RPCS3 reads 208KB of adv_voice_talk.cvm; if we read 0 the player
     * never requests it (upstream state-machine gate). */
    const char* p = s_files[fd].path;
    /* YZ_FS_TRACE (2026-07-03 s8, diag — the pxd spurious-completion probe):
     * log EVERY read/lseek/fstat, not just stream containers; the boundary
     * streams read an archive fd whose path misses the is_stream filters. */
    int fs_trace = yz_fs_trace_mode();
    int path_stream =
        (p && (strstr(p, ".cvm") || strstr(p, ".sfd") || strstr(p, "/stream/") || strstr(p, "/movie/"))) ? 1 : 0;
    int is_stream = fs_trace || path_stream;
    long long off_before = -1;
    if (is_stream && s_files[fd].host_fp) off_before = (long long)
#ifdef _MSC_VER
        _ftelli64(s_files[fd].host_fp);
#else
        ftello(s_files[fd].host_fp);
#endif

    if (s_files[fd].host_fp) {
        bytes_read = (u64)fread(buf, 1, (size_t)nbytes, s_files[fd].host_fp);
    }
    if (!s_files[fd].read_seen) {
        /* s30 first-read marker (see FsFileSlot.read_seen) — stderr on
         * purpose: stdout is the ledger-#67 treatment under test. tms =
         * GetTickCount64 wall clock so stage cadence is measurable (the
         * .err stream has no other timestamps). */
        s_files[fd].read_seen = 1;
#ifdef _WIN32
        fprintf(stderr, "[cellFs] t%u FIRST-READ tms=%llu fd=%d '%s' nbytes=0x%llX -> 0x%llX\n",
                yz_thread_current_id(), (unsigned long long)GetTickCount64(),
                fd, p ? p : "?",
                (unsigned long long)nbytes, (unsigned long long)bytes_read);
#else
        fprintf(stderr, "[cellFs] t%u FIRST-READ fd=%d '%s' nbytes=0x%llX -> 0x%llX\n",
                yz_thread_current_id(), fd, p ? p : "?",
                (unsigned long long)nbytes, (unsigned long long)bytes_read);
#endif
    }
    /* s29 position finding: the latency model must run POST-result ("the read
     * took longer"), not at entry ("the call started later") — a concurrent
     * observer only sees the former. Entry-side floors measured no-effect
     * (s29lat/s29lat2). */
    yz_fs_lat_wait();

    if (is_stream) {
        const unsigned char* b = (const unsigned char*)buf;
        if (fs_trace == 2 && !path_stream) {
            /* dark mode: same formatting work, private sink, zero stdout
             * (path-stream files keep their always-on stdout print so the
             * boot's stdout profile stays identical to an unflagged run) */
            char tl[256];
            snprintf(tl, sizeof(tl), "[cellFs] t%u Read(fd=%d '%s') off=0x%llX nbytes=0x%llX -> 0x%llX  hdr=%02X%02X%02X%02X %02X%02X%02X%02X (%c%c%c%c)\n",
                   yz_thread_current_id(),
                   fd, p, (unsigned long long)off_before, (unsigned long long)nbytes,
                   (unsigned long long)bytes_read,
                   bytes_read>0?b[0]:0, bytes_read>1?b[1]:0, bytes_read>2?b[2]:0, bytes_read>3?b[3]:0,
                   bytes_read>4?b[4]:0, bytes_read>5?b[5]:0, bytes_read>6?b[6]:0, bytes_read>7?b[7]:0,
                   (bytes_read>0&&b[0]>=32&&b[0]<127)?b[0]:'.', (bytes_read>1&&b[1]>=32&&b[1]<127)?b[1]:'.',
                   (bytes_read>2&&b[2]>=32&&b[2]<127)?b[2]:'.', (bytes_read>3&&b[3]>=32&&b[3]<127)?b[3]:'.');
            yz_fs_dark_puts(tl);
        } else
        printf("[cellFs] t%u Read(fd=%d '%s') off=0x%llX nbytes=0x%llX -> 0x%llX  hdr=%02X%02X%02X%02X %02X%02X%02X%02X (%c%c%c%c)\n",
               yz_thread_current_id(),
               fd, p, (unsigned long long)off_before, (unsigned long long)nbytes,
               (unsigned long long)bytes_read,
               bytes_read>0?b[0]:0, bytes_read>1?b[1]:0, bytes_read>2?b[2]:0, bytes_read>3?b[3]:0,
               bytes_read>4?b[4]:0, bytes_read>5?b[5]:0, bytes_read>6?b[6]:0, bytes_read>7?b[7]:0,
               (bytes_read>0&&b[0]>=32&&b[0]<127)?b[0]:'.', (bytes_read>1&&b[1]>=32&&b[1]<127)?b[1]:'.',
               (bytes_read>2&&b[2]>=32&&b[2]<127)?b[2]:'.', (bytes_read>3&&b[3]>=32&&b[3]<127)?b[3]:'.');
    }

    /* ADX HLE (env YZ_ADX_HLE, 2026-07-04): clean-room host decode of the CRI
     * intro-voice ADX stream, bypassing the LLE cri_audio SPU codec (measured
     * dead-on-arrival over many debugging rounds -- launches, never advances
     * the ADXM progress fields; pure codec data transforms may be HLE'd).
     * Bypass ONLY the decode; everything else (file read, CRI
     * player state machine, SPURS dispatch) stays real/LLE. See
     * yz_adx_hle_on_read() below for the accumulation/decode/publish logic. */
    if (bytes_read > 0 && p && (strstr(p, "adv_voice") || strstr(p, ".cvm")) &&
        getenv("YZ_ADX_HLE")) {
        extern void yz_adx_hle_on_read(long long file_off, const void* data, u64 len);
        yz_adx_hle_on_read(off_before, buf, bytes_read);
    }

    /* YZ_ADX_RELEASE_TEST (2026-07-04, decisive control-flow experiment):
     * YZ_ADX_HLE is measured INERT on the real intro-voice stream (it's AHX/
     * MPEG Layer II, not ADX -- adx_open() correctly rejects it, so the
     * ADXM-advance + SPURS-release calls below never fire). Before spending a
     * session writing an AHX/MPEG decoder, test the cheap, separable question:
     * if those two calls DID fire (with fabricated/silent progress, zero real
     * PCM), does t1 actually leave its func_02015C2C SPURS poll at all? This
     * isolates "is the SPURS-release lever even the right one" from "can we
     * decode AHX" -- unconditional on decode success, gated on its own
     * flag (independent of YZ_ADX_HLE) so the two experiments don't conflate.
     * Fires on every stream read past the container header (matches the real
     * per-block cadence loosely; exact rate doesn't matter for this test). */
    if (bytes_read > 0 && p && (strstr(p, "adv_voice") || strstr(p, ".cvm")) &&
        getenv("YZ_ADX_RELEASE_TEST")) {
        extern void yz_adx_release_test_tick(void);
        yz_adx_release_test_tick();
    }

    if (nread)
        *nread = ps3_bswap64(bytes_read);

    return CELL_OK;
}

/* ===========================================================================
 * ADX HLE (env YZ_ADX_HLE) -- clean-room CRI ADX decode + ADXM/SPURS release
 *
 * The intro voice container (adv_voice_talk.cvm) is a CVM archive: CVMH
 * header @0, ZONE table @0x800, a real ISO9660 volume descriptor at @0x9800
 * (CD001, shifted +0x1800 vs a standard ISO -- confirmed 2026-07-04 by
 * parsing the PVD root directory record: LBA 20, and walking its entries).
 * pt29's "raw ADX bytestream 0xB800..0x27800" was the byte RANGE the game's
 * *directory-parse* reads covered, NOT a fixed ADX-header offset -- there is
 * no valid ADX header in that range (verified: the only 0x8000 byte pairs in
 * it are coincidental, e.g. offset 0xBF8F decodes to impossible fields --
 * bitdepth 172, channels 110). **ROOT DISCOVERY (2026-07-04): the root
 * directory's actual audio entries are named `*.AHX;1` (e.g.
 * AKIYAMA_01_100_005.AHX), not .ADX.** AHX is a DIFFERENT CRI codec: the
 * on-disk header (0x8000 magic, copyright_offset field) matches ADX's
 * container shape, but encoding_type=0x11 (not 2/3) and the payload right
 * after the copyright tag is an MPEG-1 Layer II frame (confirmed: bytes
 * `FF F5 E0 C0...` = an MPEG audio sync word, not ADPCM nibbles) -- publicly
 * documented as CRI's low-bitrate voice format (ADX header + MPEG-2 Layer II
 * audio). adx_open() correctly returns NULL for this stream (encoding_type
 * check rejects it) rather than silently misdecoding garbage; this HLE path
 * is consequently INERT on the real intro-voice stream until an AHX (MPEG
 * Layer II) decoder is written -- a different, larger effort than ADX
 * ADPCM. See the session report for the full chain of evidence.
 *
 * The game streams the container via ordinary cellFsRead calls (already
 * logged above); this hook mirrors every byte read into a host-side shadow
 * of the file (by absolute file offset, so out-of-order/re-reads still land
 * correctly), looks for the ADX 0x8000 magic + a valid encoding_type once
 * enough of the stream has arrived, and decodes every whole block that
 * becomes available. Decoded PCM's sole job here is to ADVANCE PROGRESS the
 * CRI player can observe -- the ADXM object's progress fields (+294/+298/
 * +29C @ 0x01613368, same fields yakuza/shims.cpp's YZ_SKIP_VOICE probe
 * already reads) and, per-decode-batch, an attempt to release t1's SPURS
 * event-flag poll (func_02015C2C / cellSpursEventFlagWait @ 0x02015F74,
 * object measured at guest EA 0x63D61720 -- see
 * yz_adx_hle_release_spurs_waiter() for the exact mechanism + its caveats).
 * NONE of this SPURS/ADXM machinery has been exercised end-to-end yet
 * (nothing ever decodes on the real stream) -- it is verified only against
 * the standalone adx_decode selftest (scratch/adx_selftest.c) and a clean
 * build/link, not against a live boot.
 * =========================================================================*/
#include "ps3emu/ps3types.h"

#define ADX_SHADOW_CAP        (512u * 1024u)   /* generous vs the measured ~112-208KB stream */
#define ADX_ADXM_EA           0x01613368u      /* pt47: ADXM progress-field object */
#define ADX_ADXM_PROGRESS_OFF 0x294u            /* +294/+298/+29C: 3 progress words (pt47 probe) */
#define ADX_SPURS_EVENTFLAG_EA 0x63D61720u     /* measured: t1's poll target in func_02015C2C */
#define ADX_SPURS_EVENTFLAG_SET_FN 0x02016010u /* libsre cellSpursEventFlagSet(eventFlag,bits) -- scratch/libsre_lle_map.txt */

static unsigned char s_adx_shadow[ADX_SHADOW_CAP];
static u64            s_adx_shadow_hi;     /* highest byte offset written + 1 */
static int             s_adx_have_header;
static AdxDecoder*      s_adx_dec;
static u32              s_adx_next_decode_off;  /* next block-aligned offset to decode */
static u32              s_adx_pcm_frames_total;  /* running decode progress, for ADXM fields */

/* Call the guest cellSpursEventFlagSet(eventFlag_ea, bits) directly as a host
 * function (yz_lookup_func resolves 0x02016010 -> the lifted libsre body).
 * This function is a self-contained lwarx/stwcx bitmask update over its
 * r3/r4 args + the stack (no r2/TOC-relative loads observed in the lift --
 * verified by reading recomp_prx/libsre_recomp_000.cpp around func_02016010),
 * so a synthetic one-shot context is safe, mirroring the pattern main.cpp's
 * guest_caller() and dispatch.cpp's yz_mwply_probe_dispatch() already use to
 * invoke isolated libsre/game leaf functions without a full OPD/TOC setup.
 * UNPROVEN which bits t1's cellSpursEventFlagWait(bits=r5 at the call site)
 * actually waits on -- we log that value (once) and set ALL bits (~0u) as
 * the conservative "release every waiter" signal; the boot test measures
 * whether this is the right lever. */
/* CRASH FIX (2026-07-04, found by the YZ_ADX_RELEASE_TEST boot): func_02016010
 * is NOT a stack-free leaf -- its prologue pushes a REAL frame through
 * ctx->gpr[1] (saves gpr[18..31] to [gpr[1]+0xA0..0x108], then
 * gpr[1] += -0x110). The first synthetic-context attempt zeroed gpr[1], so
 * the prologue wrote guest EA (0 - 0x110) = 0xFFFFFFF0-ish (observed fault:
 * "writing host addr ... guest 0xFFFFFEF0") -> immediate access violation on
 * tid=11 (0xC0000005), crash log showed func_02016010 +0x1AC in the call
 * chain right above the fault. Fix: give the synthetic context a REAL guest
 * stack (lazily allocated once via vm_stack_allocate, same mechanism
 * yakuza/main.cpp's guest_caller() uses for HLE->guest calls), not a
 * zero-filled scratch buffer. */
#include "../../runtime/memory/vm.h"

static void yz_adx_hle_release_spurs_waiter(void)
{
    extern void* yz_lookup_func(u32 guest_addr);
    typedef void (*yz_ppu_fn)(void* ctx);
    static void* fn_cache = (void*)-1;   /* -1 = not yet looked up */
    if (fn_cache == (void*)-1) {
        fn_cache = (void*)yz_lookup_func(ADX_SPURS_EVENTFLAG_SET_FN);
        if (!fn_cache)
            printf("[adx-hle] cellSpursEventFlagSet (0x%08X) not in the function table -- "
                   "cannot release t1's SPURS poll; ADXM progress will still advance.\n",
                   ADX_SPURS_EVENTFLAG_SET_FN);
    }
    if (!fn_cache) return;

    /* Lazily allocate a dedicated guest stack for this synthetic call (once
     * per process -- this helper is only ever called from HLE/host threads,
     * not from a guest PPU thread's own stack, so a single static stack is
     * safe: calls here are not reentrant with each other in practice, and
     * even if they were, each call fully unwinds before returning). Same
     * null-back-chain-terminator precaution as main.cpp's guest_caller(),
     * in case something in the callee ever walks the stack. */
    static u32 call_stack_top;
    if (!call_stack_top) {
        static vm_stack_alloc sa;
        static int sa_init = 0;
        if (!sa_init) { sa_init = 1; vm_stack_alloc_init(&sa); }
        u32 base = vm_stack_allocate(&sa, 64 * 1024);
        if (!base) {
            printf("[adx-hle] guest stack alloc failed -- cannot call cellSpursEventFlagSet\n");
            return;
        }
        call_stack_top = (base + 64 * 1024 - 0x100) & ~0xFu;
        vm_write32(call_stack_top, 0);       /* null back-chain terminator (low word) */
        vm_write32(call_stack_top + 4, 0);   /* (high word, in case a walker reads 64-bit) */
    }

    /* Minimal synthetic ppu_context: gpr[1] = real guest stack, gpr[3]/gpr[4]
     * = args; everything else zeroed so no stale reservation/CR state leaks
     * in (same caution as main.cpp's guest_caller). */
    unsigned char ctx_buf[4096];
    memset(ctx_buf, 0, sizeof(ctx_buf));
    /* ppu_context's exact layout is owned by ppu_recomp.h/ppu_context.h and
     * differs between the recompiled-code view and the runtime view (see the
     * NOTE at the top of yakuza/shims.cpp); gpr[] is first in both, so a
     * uint64_t[] alias at offset 0 reaches gpr[1]/gpr[3]/gpr[4] safely
     * regardless of which layout this TU's linked object uses, without
     * needing to include either ppu_context header here (cellFs.c is a
     * plain HLE module). */
    uint64_t* gpr = (uint64_t*)ctx_buf;
    gpr[1] = call_stack_top;
    gpr[3] = ADX_SPURS_EVENTFLAG_EA;
    gpr[4] = ~0u;   /* bits: release-all (conservative; see caveat above) */

    ((yz_ppu_fn)fn_cache)((void*)ctx_buf);

    static int logged = 0;
    if (!logged) { logged = 1;
        printf("[adx-hle] called cellSpursEventFlagSet(ea=0x%08X, bits=0xFFFFFFFF)\n",
               ADX_SPURS_EVENTFLAG_EA);
    }
}

/* Advance the ADXM object's progress fields so the CRI player's readiness
 * predicate (whatever it polls -- unconfirmed exact semantics, pt47) sees
 * forward motion proportional to real decoded audio, not a one-shot force. */
static void yz_adx_hle_advance_adxm(u32 pcm_frames_total)
{
    vm_write32(ADX_ADXM_EA + ADX_ADXM_PROGRESS_OFF + 0, pcm_frames_total);       /* +294 */
    vm_write32(ADX_ADXM_EA + ADX_ADXM_PROGRESS_OFF + 4, pcm_frames_total);       /* +298 */
    vm_write32(ADX_ADXM_EA + ADX_ADXM_PROGRESS_OFF + 8, 0xFFFFFFFFu);            /* +29C: "ready" sentinel, same value YZ_SKIP_VOICE forces */
}

void yz_adx_hle_on_read(long long file_off, const void* data, u64 len)
{
    if (file_off < 0 || len == 0) return;
    u64 end = (u64)file_off + len;
    if (end > ADX_SHADOW_CAP) end = ADX_SHADOW_CAP;   /* clamp -- generous cap, see above */
    if ((u64)file_off >= end) return;

    memcpy(s_adx_shadow + (u64)file_off, data, end - (u64)file_off);
    if (end > s_adx_shadow_hi) s_adx_shadow_hi = end;

    if (!s_adx_dec) {
        /* Scan the shadow for the ADX 0x8000 magic (pt29 measured it landing
         * around byte 0xB800 in this container, but scan generously in case
         * of container-layout drift rather than hardcoding that offset). */
        for (u32 off = 0; off + 20 <= s_adx_shadow_hi && off < ADX_SHADOW_CAP - 20; off++) {
            if (s_adx_shadow[off] == 0x80 && s_adx_shadow[off + 1] == 0x00) {
                AdxDecoder* d = adx_open(s_adx_shadow + off, s_adx_shadow_hi - off);
                if (d) {
                    s_adx_dec = d;
                    s_adx_next_decode_off = off + adx_data_offset(d);
                    s_adx_have_header = 1;
                    printf("[adx-hle] ADX header found at shadow offset 0x%X: ch=%d rate=%d "
                           "block=%u data_off=%u total_samples=%u\n",
                           off, adx_channels(d), adx_sample_rate(d), adx_block_size(d),
                           adx_data_offset(d), adx_total_samples(d));
                    break;
                }
            }
        }
        if (!s_adx_dec) return;
    }

    /* Decode every whole block now available in the shadow. */
    u32 block_bytes = adx_block_size(s_adx_dec) * (u32)adx_channels(s_adx_dec);
    if (block_bytes == 0) return;
    int16_t pcm[256];
    int decoded_any = 0;
    while (s_adx_next_decode_off + block_bytes <= s_adx_shadow_hi) {
        int n = adx_decode_block(s_adx_dec, s_adx_shadow, s_adx_shadow_hi,
                                  s_adx_next_decode_off, pcm,
                                  (int)(sizeof(pcm) / sizeof(pcm[0]) / adx_channels(s_adx_dec)));
        if (n < 0) break;   /* truncated/misaligned -- wait for more bytes */
        s_adx_next_decode_off += block_bytes;
        s_adx_pcm_frames_total += (u32)n;
        decoded_any = 1;
    }

    if (decoded_any) {
        yz_adx_hle_advance_adxm(s_adx_pcm_frames_total);
        yz_adx_hle_release_spurs_waiter();

        static long batches = 0;
        if (++batches <= 20 || (batches % 100) == 0)
            printf("[adx-hle] decoded batch #%ld: total_pcm_frames=%u next_off=0x%X shadow_hi=0x%llX\n",
                   batches, s_adx_pcm_frames_total, s_adx_next_decode_off,
                   (unsigned long long)s_adx_shadow_hi);
    }
}

/* ===========================================================================
 * YZ_ADX_RELEASE_TEST (2026-07-04) -- decisive control-flow experiment
 *
 * Question: is cellSpursEventFlagSet(0x63D61720,~0) + advancing the ADXM
 * progress fields (0x01613368+0x294/+298/+29C) EVEN THE RIGHT LEVER to move
 * t1 out of its func_02015C2C SPURS poll? Fully separable from "can we decode
 * AHX" -- fires the same two calls YZ_ADX_HLE would have fired on a real
 * decode, but unconditionally (no header parse, no real PCM: fabricated,
 * monotonically increasing "blocks decoded" progress only). A silent/zero-
 * data release is fine for this test; we are testing the control-flow signal,
 * not audio correctness. Independent flag from YZ_ADX_HLE so the two
 * experiments (decode-format-correctness vs release-mechanism-correctness)
 * don't get conflated in one boot's results.
 * =========================================================================*/
void yz_adx_release_test_tick(void)
{
    static u32 fake_progress = 0;
    fake_progress++;   /* monotonic increasing "N blocks decoded" stand-in */

    yz_adx_hle_advance_adxm(fake_progress);
    yz_adx_hle_release_spurs_waiter();

    static long ticks = 0;
    if (++ticks <= 20 || (ticks % 100) == 0)
        printf("[adx-release-test] tick #%ld: fake_progress=%u (ADXM advanced + "
               "cellSpursEventFlagSet fired on 0x%08X)\n",
               ticks, fake_progress, ADX_SPURS_EVENTFLAG_EA);
}

/* NID: 0x1E9B6714 */
s32 cellFsWrite(CellFsFd fd, const void* buf, u64 nbytes, u64* nwrite)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (!buf)
        return CELL_EFAULT;

    u64 bytes_written = 0;

    if (s_files[fd].host_fp) {
        bytes_written = (u64)fwrite(buf, 1, (size_t)nbytes, s_files[fd].host_fp);
        fflush(s_files[fd].host_fp);
    }

    if (nwrite)
        *nwrite = ps3_bswap64(bytes_written);

    return CELL_OK;
}

/* NID: 0xA397D042 */
s32 cellFsLseek(CellFsFd fd, s64 offset, s32 whence, u64* pos)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    yz_fs_lat_wait();
    if (s_files[fd].host_fp) {
        int host_whence = SEEK_SET;
        if (whence == CELL_FS_SEEK_CUR) host_whence = SEEK_CUR;
        if (whence == CELL_FS_SEEK_END) host_whence = SEEK_END;

#ifdef _MSC_VER
        _fseeki64(s_files[fd].host_fp, offset, host_whence);
        s64 cur = _ftelli64(s_files[fd].host_fp);
#else
        fseeko(s_files[fd].host_fp, (off_t)offset, host_whence);
        s64 cur = (s64)ftello(s_files[fd].host_fp);
#endif
        { int fs_trace = yz_fs_trace_mode();
          if (fs_trace == 2) {
              char tl[256];
              snprintf(tl, sizeof(tl), "[cellFs] Lseek(fd=%d '%s') off=0x%llX whence=%d -> pos=0x%llX\n",
                     fd, s_files[fd].path ? s_files[fd].path : "?",
                     (unsigned long long)offset, whence, (unsigned long long)cur);
              yz_fs_dark_puts(tl);
          } else if (fs_trace)
              printf("[cellFs] Lseek(fd=%d '%s') off=0x%llX whence=%d -> pos=0x%llX\n",
                     fd, s_files[fd].path ? s_files[fd].path : "?",
                     (unsigned long long)offset, whence, (unsigned long long)cur); }
        if (pos)
            *pos = ps3_bswap64((u64)cur);
    } else {
        if (pos)
            *pos = 0;
    }

    return CELL_OK;
}

/* NID: 0xEF3BBD5A */
s32 cellFsFstat(CellFsFd fd, CellFsStat* sb)
{
    printf("[cellFs] Fstat(fd=%d)\n", fd);
    yz_fs_lat_wait();

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;
    if (!sb)
        return CELL_EFAULT;

    if (s_files[fd].host_fp) {
#ifdef _WIN32
        int file_no = _fileno(s_files[fd].host_fp);
        HOST_STAT_T hst;
        if (HOST_FSTAT(file_no, &hst) == 0) {
            { int fs_trace = yz_fs_trace_mode();
              if (fs_trace == 2) {
                  char tl[256];
                  snprintf(tl, sizeof(tl), "[cellFs] Fstat(fd=%d '%s') -> st_size=0x%llX\n",
                         fd, s_files[fd].path ? s_files[fd].path : "?",
                         (unsigned long long)hst.st_size);
                  yz_fs_dark_puts(tl);
              } else if (fs_trace)
                  printf("[cellFs] Fstat(fd=%d '%s') -> st_size=0x%llX\n",
                         fd, s_files[fd].path ? s_files[fd].path : "?",
                         (unsigned long long)hst.st_size); }
            fill_cellfs_stat(sb, &hst);
            return CELL_OK;
        }
#else
        int file_no = fileno(s_files[fd].host_fp);
        HOST_STAT_T hst;
        if (HOST_FSTAT(file_no, &hst) == 0) {
            fill_cellfs_stat(sb, &hst);
            return CELL_OK;
        }
#endif
    }

    /* Fallback */
    memset(sb, 0, sizeof(CellFsStat));
    sb->st_mode = CELL_FS_S_IFREG | CELL_FS_S_IRUSR | CELL_FS_S_IWUSR;
    sb->st_blksize = 4096;
    cellfs_stat_to_be(sb);
    return CELL_OK;
}

/* NID: 0x2CB51F0D */
s32 cellFsStat(const char* path, CellFsStat* sb)
{
    printf("[cellFs] Stat(path='%s')\n", path ? path : "<null>");

    if (!path || !sb)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    HOST_STAT_T hst;
    if (HOST_STAT(host_path, &hst) != 0) {
        printf("[cellFs] Stat: host stat('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }

    fill_cellfs_stat(sb, &hst);
    return CELL_OK;
}

/* NID: 0x6D3BB15B */
s32 cellFsTruncate(const char* path, u64 size)
{
    printf("[cellFs] Truncate(path='%s', size=%llu)\n", path ? path : "<null>",
           (unsigned long long)size);

    if (!path)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

#ifdef _WIN32
    HANDLE hFile = CreateFileA(host_path, GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return (s32)CELL_ENOENT;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
    SetEndOfFile(hFile);
    CloseHandle(hFile);
#else
    if (truncate(host_path, (off_t)size) != 0)
        return (s32)CELL_ENOENT;
#endif

    return CELL_OK;
}

/* NID: 0x82D3AB53 */
s32 cellFsFtruncate(CellFsFd fd, u64 size)
{
    printf("[cellFs] Ftruncate(fd=%d, size=%llu)\n", fd, (unsigned long long)size);

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (s_files[fd].host_fp) {
        fflush(s_files[fd].host_fp);
#ifdef _WIN32
        int file_no = _fileno(s_files[fd].host_fp);
        _chsize_s(file_no, (long long)size);
#else
        int file_no = fileno(s_files[fd].host_fp);
        ftruncate(file_no, (off_t)size);
#endif
    }

    return CELL_OK;
}

/* NID: 0xC1C507E7 */
s32 cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size)
{
    printf("[cellFs] GetBlockSize(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    if (sector_size) *sector_size = ps3_bswap64(512);
    if (block_size)  *block_size  = ps3_bswap64(4096);

    return CELL_OK;
}

/* NID: 0x2C2C5F71 */
s32 cellFsGetFreeSize(const char* path, u32* block_size, u64* free_block_count)
{
    printf("[cellFs] GetFreeSize(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    if (block_size) *block_size = ps3_bswap32(4096);

    /* Report ~1GB free by default */
    u64 free_blocks = (u64)(1024ULL * 1024 * 1024 / 4096);

#ifdef _WIN32
    {
        char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
        if (translate_path(path, host_path, sizeof(host_path)) == 0) {
            ULARGE_INTEGER free_bytes;
            if (GetDiskFreeSpaceExA(host_path, &free_bytes, NULL, NULL)) {
                free_blocks = (u64)(free_bytes.QuadPart / 4096);
            }
        }
    }
#endif

    if (free_block_count) *free_block_count = ps3_bswap64(free_blocks);

    return CELL_OK;
}

/* NID: 0x3F61245C */
s32 cellFsChmod(const char* path, s32 mode)
{
    printf("[cellFs] Chmod(path='%s', mode=0%o)\n", path ? path : "<null>", mode);

    if (!path)
        return CELL_EFAULT;

    /* On host we mostly ignore PS3 permission bits; just succeed */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Directory operations
 * -----------------------------------------------------------------------*/

/* NID: 0x5C74903D */
s32 cellFsOpendir(const char* path, CellFsDir* fd)
{
    printf("[cellFs] Opendir(path='%s')\n", path ? path : "<null>");

    if (!path || !fd)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    CellFsDir slot = alloc_dir();
    if (slot < 0)
        return CELL_FS_ERROR_EMFILE;

    strncpy(s_dirs[slot].host_path, host_path, CELL_FS_MAX_FS_PATH_LENGTH - 1);
    s_dirs[slot].host_path[CELL_FS_MAX_FS_PATH_LENGTH - 1] = '\0';

#ifdef _WIN32
    {
        char search_path[CELL_FS_MAX_FS_PATH_LENGTH];
        snprintf(search_path, sizeof(search_path), "%s\\*", host_path);
        s_dirs[slot].find_handle = FindFirstFileA(search_path, &s_dirs[slot].find_data);
        if (s_dirs[slot].find_handle == INVALID_HANDLE_VALUE) {
            s_dirs[slot].in_use = 0;
            printf("[cellFs] Opendir: FindFirstFile('%s') failed\n", search_path);
            return (s32)CELL_ENOENT;
        }
        s_dirs[slot].first_read = 1;
        s_dirs[slot].done = 0;
    }
#else
    s_dirs[slot].host_dir = opendir(host_path);
    if (!s_dirs[slot].host_dir) {
        s_dirs[slot].in_use = 0;
        printf("[cellFs] Opendir: opendir('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }
#endif

    *fd = (CellFsDir)ps3_bswap32((u32)slot);   /* guest reads the dir fd big-endian */
    printf("[cellFs] Opendir: dir_fd=%d -> '%s'\n", slot, host_path);
    return CELL_OK;
}

/* NID: 0x9F951810 */
/* s23 conformance fix: the real ABI (RPCS3 cellFs.cpp:91 cellFsReaddir with
 * vm::ptr<CellFsDirent>, MFF_PERFECT) fills a 258-byte CellFsDirent
 * {d_type, d_namlen, d_name[256]}, NOT a CellFsDirectoryEntry -- the old
 * body memset 308 bytes over the guest's 258-byte buffer (50-byte OOB guest
 * write) and put the name at +52 instead of +2. The stat-carrying variant
 * belongs to cellFsGetDirectoryEntries, which this game never imports. */
s32 cellFsReaddir(CellFsDir fd, CellFsDirent* entry, u64* nread)
{
    if (fd < 0 || fd >= MAX_OPEN_DIRS || !s_dirs[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (!entry || !nread)
        return CELL_EFAULT;

    { static int rd = 0; if (rd < 4) { rd++;
        fprintf(stderr, "[cellFs] Readdir(fd=%d) (real-ABI path)\n", fd); } }

#ifdef _WIN32
    if (s_dirs[fd].done) {
        *nread = 0;
        return CELL_OK;
    }

    if (!s_dirs[fd].first_read) {
        if (!FindNextFileA(s_dirs[fd].find_handle, &s_dirs[fd].find_data)) {
            s_dirs[fd].done = 1;
            *nread = 0;
            return CELL_OK;
        }
    }
    s_dirs[fd].first_read = 0;

    memset(entry, 0, sizeof(CellFsDirent));
    entry->d_type = (s_dirs[fd].find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ? CELL_FS_TYPE_DIRECTORY : CELL_FS_TYPE_REGULAR;
    strncpy(entry->d_name, s_dirs[fd].find_data.cFileName,
            CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1);
    entry->d_name[CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1] = '\0';
    entry->d_namlen = (u8)strlen(entry->d_name);

    *nread = ps3_bswap64(1);
#else
    struct dirent* de = readdir(s_dirs[fd].host_dir);
    if (!de) {
        *nread = 0;
        return CELL_OK;
    }

    memset(entry, 0, sizeof(CellFsDirent));
    entry->d_type = (de->d_type == DT_DIR) ? CELL_FS_TYPE_DIRECTORY
                                           : CELL_FS_TYPE_REGULAR;
    strncpy(entry->d_name, de->d_name, CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1);
    entry->d_name[CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1] = '\0';
    entry->d_namlen = (u8)strlen(entry->d_name);

    *nread = ps3_bswap64(1);
#endif

    return CELL_OK;
}

/* NID: 0xFF42DCC3 */
s32 cellFsClosedir(CellFsDir fd)
{
    printf("[cellFs] Closedir(fd=%d)\n", fd);

    if (fd < 0 || fd >= MAX_OPEN_DIRS || !s_dirs[fd].in_use)
        return CELL_FS_ERROR_EBADF;

#ifdef _WIN32
    if (s_dirs[fd].find_handle != INVALID_HANDLE_VALUE) {
        FindClose(s_dirs[fd].find_handle);
        s_dirs[fd].find_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (s_dirs[fd].host_dir) {
        closedir(s_dirs[fd].host_dir);
        s_dirs[fd].host_dir = NULL;
    }
#endif

    s_dirs[fd].in_use = 0;
    return CELL_OK;
}

/* NID: 0x7C1B2FCC */
s32 cellFsMkdir(const char* path, s32 mode)
{
    printf("[cellFs] Mkdir(path='%s', mode=0%o)\n", path ? path : "<null>", mode);

    if (!path)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    ensure_parent_dirs(host_path);

    int ret = HOST_MKDIR(host_path);
    if (ret != 0 && errno != EEXIST) {
        printf("[cellFs] Mkdir: mkdir('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }
    if (ret != 0 && errno == EEXIST) {
        return (s32)CELL_EEXIST;
    }

    return CELL_OK;
}

/* NID: 0xE3F6F665 */
s32 cellFsRename(const char* from, const char* to)
{
    printf("[cellFs] Rename(from='%s', to='%s')\n",
           from ? from : "<null>", to ? to : "<null>");

    if (!from || !to)
        return CELL_EFAULT;

    char host_from[CELL_FS_MAX_FS_PATH_LENGTH];
    char host_to[CELL_FS_MAX_FS_PATH_LENGTH];

    if (translate_path(from, host_from, sizeof(host_from)) != 0)
        return (s32)CELL_ENOENT;
    if (translate_path(to, host_to, sizeof(host_to)) != 0)
        return (s32)CELL_ENOENT;

    ensure_parent_dirs(host_to);

    if (rename(host_from, host_to) != 0) {
        printf("[cellFs] Rename: rename('%s', '%s') failed: %s\n",
               host_from, host_to, strerror(errno));
        return (s32)CELL_ENOENT;
    }

    return CELL_OK;
}

/* NID: 0x196CE171 */
s32 cellFsUnlink(const char* path)
{
    printf("[cellFs] Unlink(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    if (remove(host_path) != 0) {
        printf("[cellFs] Unlink: remove('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }

    return CELL_OK;
}
