/*
 * ps3recomp - LV2 syscall registration
 *
 * Calls all sys_X_init functions to populate the syscall dispatch table
 * with real HLE handlers.
 *
 * Registration order matters for conflicting syscall numbers:
 * timers are registered before events, so event handlers take precedence
 * for the colliding numbers (141, 142, 145). Timer sleep/time functions
 * remain available as direct C calls for the runtime to use.
 */

#include "lv2_syscall_table.h"
#include "sys_ppu_thread.h"
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_semaphore.h"
#include "sys_rwlock.h"
#include "sys_event.h"
#include "sys_timer.h"
#include "sys_memory.h"
#include "sys_fs.h"
#include "ps3emu/spu_fallback.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * TTY syscalls (used by PS3 CRT for debug output)
 *
 * sys_tty_read  (402) — read from TTY (stdin)
 * sys_tty_write (403) — write to TTY (stdout/stderr)
 *
 * These are among the most commonly called syscalls in CRT startup.
 * -----------------------------------------------------------------------*/

extern uint8_t* vm_base;

static int64_t sys_tty_write(ppu_context* ctx)
{
    /* s32 sys_tty_write(s32 ch, const void* buf, u32 len, u32* pwritelen) */
    uint32_t ch     = (uint32_t)ctx->gpr[3];
    uint32_t buf_ea = (uint32_t)ctx->gpr[4];
    uint32_t len    = (uint32_t)ctx->gpr[5];
    uint32_t pwr_ea = (uint32_t)ctx->gpr[6];

    (void)ch; /* channel number, ignored */

    if (buf_ea && len > 0 && vm_base) {
        /* Write guest string data to host stderr */
        fwrite(vm_base + buf_ea, 1, len, stderr);
        fflush(stderr);
    }

    /* Write back the number of bytes written */
    if (pwr_ea && vm_base) {
        uint32_t be_len = ((len >> 24) & 0xFF) | ((len >> 8) & 0xFF00) |
                          ((len << 8) & 0xFF0000) | ((len << 24) & 0xFF000000);
        memcpy(vm_base + pwr_ea, &be_len, 4);
    }

    return 0; /* CELL_OK */
}

static int64_t sys_tty_read(ppu_context* ctx)
{
    /* s32 sys_tty_read(s32 ch, void* buf, u32 len, u32* preadlen) */
    uint32_t prd_ea = (uint32_t)ctx->gpr[6];

    /* No TTY input available — return 0 bytes read */
    if (prd_ea && vm_base)
        memset(vm_base + prd_ea, 0, 4);

    return 0;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/

#define SYS_TTY_READ   402
#define SYS_TTY_WRITE  403

/* ---------------------------------------------------------------------------
 * Stateful SPU thread group tracker
 *
 * We don't execute SPU programs — SPURS job queues, SPU tasks, and raw SPU
 * threads all resolve to an empty "thread completed normally" result. But
 * the PPU-side wrappers (PhyreEngine's SPURS wrapper in particular) check
 * returned IDs, out-param cause/status fields, and per-thread exit codes
 * after every call. A flat "return 0" stub leaves the out-params as heap
 * garbage and the wrapper then throws a C++ exception.
 *
 * This tracker assigns monotonically-increasing IDs, walks a small state
 * machine, and writes all the out-params each syscall is documented to
 * set. It doesn't try to emulate actual SPU work — the group transitions
 * straight from STARTED to STOPPED with exit code 0.
 *
 * Cause values match the public Sony SDK headers:
 *   GROUP_EXIT       = 0x0001 — sys_spu_thread_group_exit() was called
 *   ALL_THREADS_EXIT = 0x0002 — all threads completed their entry fn
 *   TERMINATED       = 0x0004 — sys_spu_thread_group_terminate() fired
 * -----------------------------------------------------------------------*/

#define SPU_GROUP_STATE_INITIALIZED  0
#define SPU_GROUP_STATE_READY        1
#define SPU_GROUP_STATE_RUNNING      2
#define SPU_GROUP_STATE_STOPPED      3
#define SPU_GROUP_STATE_DESTROYED    4

#define SPU_GROUP_CAUSE_GROUP_EXIT        0x0001u
#define SPU_GROUP_CAUSE_ALL_THREADS_EXIT  0x0002u
#define SPU_GROUP_CAUSE_TERMINATED        0x0004u

#define MAX_SPU_GROUPS   32
#define MAX_SPU_THREADS  (MAX_SPU_GROUPS * 8)

typedef struct {
    int      in_use;
    uint32_t tid;            /* thread id (unique across all groups) */
    uint32_t group_id;       /* parent group */
    uint32_t index;          /* slot within the group */
    int32_t  exit_status;
    uint32_t entry_point;    /* initial SPU image entry (informational) */
} spu_thread_t;

typedef struct {
    int      in_use;
    uint32_t id;
    int      state;
    uint32_t num_threads;
    uint32_t thread_indices[8];  /* table index into s_spu_threads */
    char     name[32];
    int32_t  exit_status;        /* final ppu-side status the group reports */
    uint32_t cause;              /* how the group ended */
} spu_group_t;

static spu_group_t  s_spu_groups[MAX_SPU_GROUPS];
static spu_thread_t s_spu_threads[MAX_SPU_THREADS];
static uint32_t     s_spu_next_group_id  = 0x1000;
static uint32_t     s_spu_next_thread_id = 0x2000;
static int          s_spu_initialized    = 0;

static spu_group_t* spu_find_group(uint32_t id)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (s_spu_groups[i].in_use && s_spu_groups[i].id == id)
            return &s_spu_groups[i];
    }
    return NULL;
}

static spu_group_t* spu_alloc_group(void)
{
    for (int i = 0; i < MAX_SPU_GROUPS; i++) {
        if (!s_spu_groups[i].in_use) {
            memset(&s_spu_groups[i], 0, sizeof(s_spu_groups[i]));
            s_spu_groups[i].in_use = 1;
            s_spu_groups[i].id     = s_spu_next_group_id++;
            s_spu_groups[i].state  = SPU_GROUP_STATE_INITIALIZED;
            s_spu_groups[i].exit_status = 0;
            s_spu_groups[i].cause  = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
            return &s_spu_groups[i];
        }
    }
    return NULL;
}

static spu_thread_t* spu_find_thread(uint32_t tid)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (s_spu_threads[i].in_use && s_spu_threads[i].tid == tid)
            return &s_spu_threads[i];
    }
    return NULL;
}

static spu_thread_t* spu_alloc_thread(void)
{
    for (int i = 0; i < MAX_SPU_THREADS; i++) {
        if (!s_spu_threads[i].in_use) {
            memset(&s_spu_threads[i], 0, sizeof(s_spu_threads[i]));
            s_spu_threads[i].in_use = 1;
            s_spu_threads[i].tid    = s_spu_next_thread_id++;
            return &s_spu_threads[i];
        }
    }
    return NULL;
}

static void vm_write_be32(uint32_t guest_addr, uint32_t val)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return;
    uint8_t* p = vm_base + guest_addr;
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >>  8);
    p[3] = (uint8_t)(val);
}

static uint32_t vm_read_be32(uint32_t guest_addr)
{
    extern uint8_t* vm_base;
    if (!vm_base || !guest_addr) return 0;
    const uint8_t* p = vm_base + guest_addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) | (uint32_t)p[3];
}

/* sys_spu_initialize(nspu, nrawspu) — one-shot global init */
static int64_t sys_spu_initialize_handler(ppu_context* ctx)
{
    uint32_t nspu    = (uint32_t)ctx->gpr[3];
    uint32_t nrawspu = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[SPU] initialize(nspu=%u, nrawspu=%u)\n", nspu, nrawspu);
    fflush(stderr);
    s_spu_initialized = 1;
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_create(out_id_ea, num, name_ea, attr_ea) */
static int64_t sys_spu_thread_group_create_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t out_ea   = (uint32_t)ctx->gpr[3];
    uint32_t num      = (uint32_t)ctx->gpr[4];
    uint32_t name_ea  = (uint32_t)ctx->gpr[5];
    uint32_t attr_ea  = (uint32_t)ctx->gpr[6];

    spu_group_t* g = spu_alloc_group();
    if (!g) {
        fprintf(stderr, "[SPU] group_create: out of groups\n");
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)-1; /* EAGAIN-ish */
        return -1;
    }
    if (num > 8) num = 8;
    g->num_threads = num;

    if (name_ea && vm_base) {
        const char* src = (const char*)(vm_base + name_ea);
        size_t i = 0;
        for (; i < sizeof(g->name) - 1 && src[i]; i++)
            g->name[i] = src[i];
        g->name[i] = 0;
    }

    vm_write_be32(out_ea, g->id);

    fprintf(stderr, "[SPU] group_create -> id=0x%X num=%u name=%.31s attr=0x%08X\n",
            g->id, num, g->name, attr_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_initialize(out_tid_ea, group_id, thread_num, img_ea, attr_ea, args_ea) */
static int64_t sys_spu_thread_initialize_handler(ppu_context* ctx)
{
    uint32_t out_tid_ea = (uint32_t)ctx->gpr[3];
    uint32_t group_id   = (uint32_t)ctx->gpr[4];
    uint32_t thread_num = (uint32_t)ctx->gpr[5];
    uint32_t img_ea     = (uint32_t)ctx->gpr[6];

    spu_group_t* g = spu_find_group(group_id);
    if (!g) {
        fprintf(stderr, "[SPU] thread_init: group 0x%X not found\n", group_id);
        fflush(stderr);
        ctx->gpr[3] = (uint64_t)(int64_t)-1;
        return -1;
    }
    spu_thread_t* t = spu_alloc_thread();
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)-1;
        return -1;
    }
    t->group_id = group_id;
    t->index    = thread_num;
    /* Read entry point from the SPU image struct if available.
     * sys_spu_image layout: type/entry/segs/nsegs — entry at +4. */
    if (img_ea) t->entry_point = vm_read_be32(img_ea + 4);

    if (thread_num < 8)
        g->thread_indices[thread_num] = (uint32_t)(t - s_spu_threads);

    vm_write_be32(out_tid_ea, t->tid);

    fprintf(stderr, "[SPU] thread_init group=0x%X index=%u img=0x%08X -> tid=0x%X entry=0x%08X\n",
            group_id, thread_num, img_ea, t->tid, t->entry_point);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_start(id) */
static int64_t sys_spu_thread_group_start_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (!g) { ctx->gpr[3] = (uint64_t)(int64_t)-1; return -1; }
    g->state = SPU_GROUP_STATE_RUNNING;

    /* For each thread in the group, look up a registered PPU fallback by
     * the thread's SPU image entry point. If found, run it (synchronously,
     * for now — async execution can come later). The fallback's return
     * value becomes the thread's exit status; the worst (most negative)
     * status across threads becomes the group's status. */
    int32_t worst = 0;
    int     fallbacks_run = 0;
    for (uint32_t i = 0; i < g->num_threads && i < 8; i++) {
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!t->in_use) continue;
        void* user = NULL;
        spu_ppu_fallback_fn fb = spu_lookup_ppu_fallback(t->entry_point, &user);
        if (fb) {
            int32_t rc = fb(t->tid, /*args_ea*/ 0, /*args_size*/ 0, user);
            t->exit_status = rc;
            if (rc < worst) worst = rc;
            fallbacks_run++;
            fprintf(stderr, "[SPU] group_start id=0x%X thread tid=0x%X "
                    "entry=0x%08X -> PPU fallback rc=%d\n",
                    id, t->tid, t->entry_point, rc);
        }
    }

    g->state = SPU_GROUP_STATE_STOPPED;
    g->cause = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
    g->exit_status = worst;
    if (fallbacks_run > 0) {
        fprintf(stderr, "[SPU] group_start id=0x%X (%d PPU fallbacks ran, group status=%d)\n",
                id, fallbacks_run, worst);
    } else {
        fprintf(stderr, "[SPU] group_start id=0x%X (no fallback; instantly completed)\n", id);
    }
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_join(id, *cause, *status) */
static int64_t sys_spu_thread_group_join_handler(ppu_context* ctx)
{
    uint32_t id         = (uint32_t)ctx->gpr[3];
    uint32_t cause_ea   = (uint32_t)ctx->gpr[4];
    uint32_t status_ea  = (uint32_t)ctx->gpr[5];

    spu_group_t* g = spu_find_group(id);
    if (!g) {
        /* Unknown group id — Sony returns CELL_ESRCH but we've seen
         * games probe with stale IDs, so be lenient and fake a success. */
        vm_write_be32(cause_ea,  SPU_GROUP_CAUSE_ALL_THREADS_EXIT);
        vm_write_be32(status_ea, 0);
        fprintf(stderr, "[SPU] group_join id=0x%X (unknown, faked ok)\n", id);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }
    /* If the group was never started, mark it stopped so a subsequent
     * destroy doesn't trip a "still running" check. */
    if (g->state == SPU_GROUP_STATE_INITIALIZED ||
        g->state == SPU_GROUP_STATE_READY) {
        g->state = SPU_GROUP_STATE_STOPPED;
    }
    vm_write_be32(cause_ea,  g->cause);
    vm_write_be32(status_ea, (uint32_t)g->exit_status);

    fprintf(stderr, "[SPU] group_join id=0x%X cause=%u status=%d\n",
            id, g->cause, g->exit_status);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_destroy(id) */
static int64_t sys_spu_thread_group_destroy_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (g) {
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx < MAX_SPU_THREADS)
                s_spu_threads[idx].in_use = 0;
        }
        g->in_use = 0;
    }
    fprintf(stderr, "[SPU] group_destroy id=0x%X\n", id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_terminate(id, exit_status) */
static int64_t sys_spu_thread_group_terminate_handler(ppu_context* ctx)
{
    uint32_t id     = (uint32_t)ctx->gpr[3];
    int32_t  status = (int32_t)ctx->gpr[4];
    spu_group_t* g = spu_find_group(id);
    if (g) {
        g->state = SPU_GROUP_STATE_STOPPED;
        g->cause = SPU_GROUP_CAUSE_TERMINATED;
        g->exit_status = status;
    }
    fprintf(stderr, "[SPU] group_terminate id=0x%X status=%d\n", id, status);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_get_exit_status(tid, *status) */
static int64_t sys_spu_thread_get_exit_status_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t status_ea = (uint32_t)ctx->gpr[4];
    spu_thread_t* t = spu_find_thread(tid);
    vm_write_be32(status_ea, t ? (uint32_t)t->exit_status : 0);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_set_argument(tid, arg_ea) — doesn't affect us, log only */
static int64_t sys_spu_thread_set_argument_handler(ppu_context* ctx)
{
    fprintf(stderr, "[SPU] thread_set_argument tid=0x%llX arg=0x%llX\n",
            (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4]);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_image_import(*img, *source, type) — just log entry & return success.
 * We could parse the SPU ELF header and write entry into the image struct,
 * but no real use without SPU execution; zero-initialize so downstream
 * reads see a valid-looking image. */
static int64_t sys_spu_image_import_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    if (img_ea && vm_base) {
        memset(vm_base + img_ea, 0, 16);
        /* sys_spu_image.type = 0 (SYS_SPU_IMAGE_TYPE_KERNEL), entry=0, segs=0, nsegs=0 */
    }
    fprintf(stderr, "[SPU] image_import img=0x%08X src=0x%08X\n", img_ea, src_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_image_open(*img, *path) — load an SPU ELF from the VFS, parse its
 * ELF32 header, and write the entry point + USER image type into the
 * sys_spu_image struct. The actual segment/code data isn't materialised
 * (we don't execute SPU); we only need entry to be correct so the SPU
 * PPU-fallback registry (ps3emu/spu_fallback.h) can match jobs by entry.
 *
 * sys_spu_image layout (16 bytes):
 *   +0  type    : u32  (0 = KERNEL, 1 = USER)
 *   +4  entry   : u32
 *   +8  segs    : u32 (EA of segment array, 0 if not materialised)
 *   +12 nsegs   : u32
 */
static int64_t sys_spu_image_open_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t img_ea  = (uint32_t)ctx->gpr[3];
    uint32_t path_ea = (uint32_t)ctx->gpr[4];

    if (img_ea && vm_base) {
        memset(vm_base + img_ea, 0, 16);
        vm_write_be32(img_ea + 0, 1);        /* type = USER */
    }

    if (!path_ea || !vm_base) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path=NULL — empty image\n", img_ea);
        fflush(stderr);
        ctx->gpr[3] = 0;
        return 0;
    }

    const char* ps3_path = (const char*)(vm_base + path_ea);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    FILE* f = fopen(host_path, "rb");
    if (!f) {
        fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' (host: %s) — open failed\n",
                img_ea, ps3_path, host_path);
        fflush(stderr);
        /* Sony returns CELL_ENOENT for missing SPU images. Games often
         * pre-check, so a soft success keeps them moving. */
        ctx->gpr[3] = 0;
        return 0;
    }

    /* ELF32 header is 52 bytes. We need:
     *   +16  e_type    (2 bytes)   2 = ET_EXEC
     *   +18  e_machine (2 bytes)   23 = EM_SPU
     *   +24  e_entry   (4 bytes)
     */
    uint8_t hdr[52];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);

    uint32_t entry = 0;
    int valid_elf = 0;
    if (got >= 52 && hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        valid_elf = 1;
        /* Big-endian on PS3 */
        entry = ((uint32_t)hdr[24] << 24) |
                ((uint32_t)hdr[25] << 16) |
                ((uint32_t)hdr[26] <<  8) |
                ((uint32_t)hdr[27]);
    }

    if (img_ea && vm_base) {
        vm_write_be32(img_ea + 4, entry);
    }

    fprintf(stderr, "[SPU] image_open img=0x%08X path='%s' entry=0x%08X%s\n",
            img_ea, ps3_path, entry, valid_elf ? "" : " (header invalid — entry left 0)");
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* Catch-all stub for SPU syscalls we don't model individually yet. */
static int64_t sys_spu_thread_stub(ppu_context* ctx)
{
    (void)ctx;
    ctx->gpr[3] = 0;
    return 0;
}

void lv2_register_all_syscalls(lv2_syscall_table* tbl)
{
    /* Initialize the table with unimplemented stubs first */
    lv2_syscall_table_init(tbl);

    /* Thread management */
    sys_ppu_thread_init(tbl);

    /* Synchronization primitives */
    sys_mutex_init(tbl);
    sys_cond_init(tbl);
    sys_semaphore_init(tbl);
    sys_rwlock_init(tbl);

    /* Timer and time (registered before events so event handlers
     * override the conflicting syscall numbers 141, 142, 145) */
    sys_timer_init(tbl);

    /* Event queues, ports, and flags */
    sys_event_init(tbl);

    /* Memory management */
    sys_memory_init(tbl);

    /* Filesystem */
    sys_fs_init(tbl);

    /* TTY (debug console I/O — used by CRT startup) */
    lv2_syscall_register(tbl, SYS_TTY_READ,  sys_tty_read);
    lv2_syscall_register(tbl, SYS_TTY_WRITE, sys_tty_write);

    /* SPU syscalls — we don't execute SPU code but the PPU-side wrappers
     * need consistent IDs and out-params. See the stateful group tracker
     * above for contract notes. */
    lv2_syscall_register(tbl, 169,                            sys_spu_thread_stub); /* deprecated */
    lv2_syscall_register(tbl, SYS_SPU_INITIALIZE,             sys_spu_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_OPEN,             sys_spu_image_open_handler);
    lv2_syscall_register(tbl, SYS_SPU_IMAGE_CLOSE,            sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CREATE,    sys_spu_thread_group_create_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DESTROY,   sys_spu_thread_group_destroy_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_START,     sys_spu_thread_group_start_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_SUSPEND,   sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_RESUME,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_YIELD,     sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_TERMINATE, sys_spu_thread_group_terminate_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_JOIN,      sys_spu_thread_group_join_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_INITIALIZE,      sys_spu_thread_initialize_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_SET_ARGUMENT,    sys_spu_thread_set_argument_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GET_EXIT_STATUS, sys_spu_thread_get_exit_status_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_CONNECT_EVENT,   sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_DISCONNECT_EVENT,sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT, sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT, sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_LS,        sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_READ_LS,         sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SNR,       sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_BIND_QUEUE,      sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_UNBIND_QUEUE,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS, sys_spu_thread_stub);
}
