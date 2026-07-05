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
#include "sys_vm.h"
#include "sys_fs.h"
#include "ps3emu/spu_fallback.h"
#include "sys_event.h"

#include <stdio.h>
#include <stdlib.h>  /* calloc/free: without the prototype MSVC assumed int
                        and truncated the 64-bit local_store pointer */
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

#ifdef _WIN32
#  include <windows.h>
typedef HANDLE spu_thread_handle_t;
typedef HANDLE spu_thread_event_t;
#else
#  include <pthread.h>
typedef pthread_t spu_thread_handle_t;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             done;
} spu_thread_event_t;
#endif

typedef struct {
    int      in_use;
    uint32_t tid;            /* thread id (unique across all groups) */
    uint32_t group_id;       /* parent group */
    uint32_t index;          /* slot within the group */
    int32_t  exit_status;
    uint32_t entry_point;    /* initial SPU image entry (informational) */
    uint32_t img_ea;         /* guest sys_spu_image struct (segment table) */
    /* Thread arguments captured AT sys_spu_thread_initialize time (lv2
     * copies them then — games reuse one guest block for all threads,
     * rewriting it between calls; reading it lazily hands every thread
     * the last thread's values. Observed live with SPURS spu_num). */
    uint64_t args[4];
    /* Real SPU execution (lifted images): full architectural context.
     * Allocated at group_start when the entry resolves to lifted code. */
    struct spu_context* sctx;
    /* Args block passed via sys_spu_thread_initialize (.args_ea) and
     * sys_spu_thread_set_argument (per-thread). For SPURS this holds the
     * 4 register-style args (arg1..arg4) packed into a guest struct.
     * The PPU fallback receives args_ea so it can decode whatever format
     * the registered job expects. */
    uint32_t args_ea;
    uint32_t args_size;
    /* Async fallback execution. host_thread is set when group_start spawned
     * a host thread for this SPU thread's PPU fallback; finish_event is
     * signalled when the handler returns; running indicates the thread is
     * still in flight. group_join waits on finish_event for each running
     * thread. */
    spu_thread_handle_t host_thread;
    spu_thread_event_t  finish_event;
    int                 running;
    spu_ppu_fallback_fn fb_handler;
    void*               fb_user;
    /* Virtual local store. Real SPU has 256 KB. Allocated lazily on first
     * sys_spu_thread_write_ls / read_ls. PPU fallbacks can also reach this
     * via the public spu_thread_get_local_store() helper, simulating the
     * common pattern where the PPU writes job state into LS, the SPU runs
     * and writes results back to LS, then PPU reads them. */
    uint8_t*            local_store;
    /* SPU run-control configuration (sc-187 sys_spu_thread_set_spu_cfg):
     * bit 0 = SNR1 in OR mode, bit 1 = SNR2 in OR mode (CBE Registers v1.5
     * SPU_Cfg; default 0 = overwrite mode for both). Read by sc-184
     * write_snr below. */
    uint64_t            spu_cfg;
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
    /* Event queue connected via sys_spu_thread_group_connect_event[_all_threads].
     * When the group transitions to STOPPED (in group_join), an event is pushed
     * into this queue with source = SYS_SPU_THREAD_GROUP_EVENT (0x100..) so
     * PPU code blocked on sys_event_queue_receive wakes up. */
    uint32_t event_queue_id;
    /* SPU->PPU user-event ports: connect_event_all_threads(req, *spup) assigns a
     * free port from `req`, binds the queue to it, and returns the port in *spup.
     * spup_queue[port] = the connected event-queue id (0 = unconnected). The SPU's
     * outbound interrupt mailbox (SPU_WrOutIntrMbox) delivers a class-2 event to
     * the queue on its port. (2026-06-21 pt29: this was unimplemented -> SPU->PPU
     * events were silently dropped -> SPURS/CRI coordination stalled.) */
    uint32_t spup_queue[64];
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

/* Event queue bound to an SPU->PPU user-event port of a group (0 = none).
 * Used by spu_wrch(SPU_WrOutIntrMbox) to deliver sys_spu_thread_send_event /
 * throw_event class-2 events (the pt29 "NOT forwarded yet" gap). */
uint32_t spu_group_spup_queue(uint32_t group_id, uint32_t spup)
{
    spu_group_t* g = spu_find_group(group_id);
    if (!g || spup >= 64) return 0;
    return g->spup_queue[spup];
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

/* sys_spu_thread_group_create(out_id_ea, num, prio, attr_ea)
 *
 * lv2 signature per RPCS3 sys_spu.h: r5 is the group PRIORITY (SPURS
 * passes 250), NOT a name pointer. The name lives inside the attribute
 * struct, reduced_sys_spu_thread_group_attribute (BE):
 *   +0 u32 nsize (name length incl. NUL), +4 u32 name ptr, +8 s32 type. */
static int64_t sys_spu_thread_group_create_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t out_ea   = (uint32_t)ctx->gpr[3];
    uint32_t num      = (uint32_t)ctx->gpr[4];
    int32_t  prio     = (int32_t)ctx->gpr[5];
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

    int32_t  gtype   = 0;
    uint32_t name_ea = 0;
    if (attr_ea) {
        uint32_t nsize = vm_read_be32(attr_ea + 0);
        name_ea        = vm_read_be32(attr_ea + 4);
        gtype          = (int32_t)vm_read_be32(attr_ea + 8);
        if (!nsize) name_ea = 0;
    }
    if (name_ea && vm_base) {
        const char* src = (const char*)(vm_base + name_ea);
        size_t i = 0;
        for (; i < sizeof(g->name) - 1 && src[i]; i++)
            g->name[i] = src[i];
        g->name[i] = 0;
    }

    vm_write_be32(out_ea, g->id);

    fprintf(stderr, "[SPU] group_create -> id=0x%X num=%u prio=%d type=0x%X name=%.31s\n",
            g->id, num, prio, gtype, g->name);
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
    /* attr_ea         = (uint32_t)ctx->gpr[7];  // unused */
    uint32_t args_ea    = (uint32_t)ctx->gpr[8];

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
    t->img_ea   = img_ea;
    /* Read entry point from the SPU image struct if available.
     * sys_spu_image layout: type/entry/segs/nsegs — entry at +4. */
    if (img_ea) t->entry_point = vm_read_be32(img_ea + 4);
    t->args_ea   = args_ea;
    t->args_size = 0;  /* not known until decoder reads it; sys_spu_thread_args is 32 B */
    /* Capture the 4 u64 args NOW (lv2 copy semantics — see struct note). */
    for (int a = 0; a < 4; a++) {
        uint64_t hi = args_ea ? vm_read_be32(args_ea + (uint32_t)a * 8)     : 0;
        uint64_t lo = args_ea ? vm_read_be32(args_ea + (uint32_t)a * 8 + 4) : 0;
        t->args[a] = (hi << 32) | lo;
    }

    if (thread_num < 8)
        g->thread_indices[thread_num] = (uint32_t)(t - s_spu_threads);

    vm_write_be32(out_tid_ea, t->tid);

    fprintf(stderr, "[SPU] thread_init group=0x%X index=%u img=0x%08X args=0x%08X -> tid=0x%X entry=0x%08X\n",
            group_id, thread_num, img_ea, args_ea, t->tid, t->entry_point);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Real SPU execution (lifted images, e.g. Sony's SPURS kernel)
 *
 * The thread's sys_spu_image segment table is deployed into a fresh
 * 256 KB local store, args land in gpr[3..6] (one 64-bit value each, in
 * the preferred doubleword — RPCS3 sys_spu.cpp: gpr[3+i] = from64(0,
 * arg[i])), pc = entry, and the lifted code runs until it stops. The
 * lifted code tail-calls between functions; the host thread gets a large
 * stack in case the compiler does not collapse a long chain.
 * -----------------------------------------------------------------------*/
#include "../spu/spu_context.h"
#include <setjmp.h>
void spu_indirect_branch(spu_context* sctx);   /* runtime/spu/spu_channels.c */
int  spu_have_function(uint32_t addr);
/* per-thread unwind target for SPU halt (spu_channels.c) */
#if defined(_MSC_VER)
extern __declspec(thread) jmp_buf* g_spu_halt_jmp;
extern __declspec(thread) jmp_buf* g_spu_restart_jmp;   /* host-call-depth guard */
extern __declspec(thread) char*    g_spu_stack_base;
#else
extern _Thread_local jmp_buf* g_spu_halt_jmp;
extern _Thread_local jmp_buf* g_spu_restart_jmp;
extern _Thread_local char*    g_spu_stack_base;
#endif

static void spu_deploy_image(spu_context* sctx, uint32_t img_ea)
{
    extern uint8_t* vm_base;
    uint32_t segs_ea = vm_read_be32(img_ea + 0x8);
    int32_t  nsegs   = (int32_t)vm_read_be32(img_ea + 0xC);
    for (int32_t i = 0; i < nsegs && segs_ea; i++) {
        uint32_t s     = segs_ea + (uint32_t)i * 0x18;
        uint32_t type  = vm_read_be32(s + 0x00);
        uint32_t ls    = vm_read_be32(s + 0x04) & SPU_LS_MASK;
        uint32_t size  = vm_read_be32(s + 0x08);
        uint32_t addr  = vm_read_be32(s + 0x10);
        if (size > SPU_LS_SIZE - ls) size = SPU_LS_SIZE - ls;
        if (type == 1)      memcpy(&sctx->ls[ls], vm_base + addr, size); /* COPY */
        else if (type == 2) memset(&sctx->ls[ls], (int)addr, size);      /* FILL */
        /* type 4 = INFO: not loaded */
    }
}

#ifdef _WIN32
static DWORD WINAPI spu_exec_thread_proc(LPVOID arg)
#else
static void* spu_exec_thread_proc(void* arg)
#endif
{
    spu_thread_t* t = (spu_thread_t*)arg;
    spu_context* sctx = t->sctx;

#ifdef _WIN32
    /* pt35e: reserve stack so the unhandled-exception filter (yz_crash_handler) can
     * still RUN on a STACK OVERFLOW. The SPURS dispatch overflows via a brsl call/return
     * nesting imbalance; without this the process dies silently (no [crash] dump, so the
     * recursion is invisible). 512 KB is plenty for the handler's trampoline-ring dump,
     * which reveals the repeating recursion cycle. */
    { ULONG guarantee = 512 * 1024; SetThreadStackGuarantee(&guarantee); }
#endif

    fprintf(stderr, "[SPU] tid=0x%X RUNNING lifted image entry=0x%05X\n",
            t->tid, sctx->pc);

    sctx->status = SPU_STATUS_RUNNING;
    jmp_buf halt_jb, restart_jb;
    char stack_base_marker;
    g_spu_halt_jmp = &halt_jb;
    g_spu_restart_jmp = &restart_jb;     /* host-call-depth guard target */
    g_spu_stack_base = &stack_base_marker;
    g_spu_trampoline_fn = 0;
    if (setjmp(halt_jb) == 0) {
        /* Re-entered by the depth-guard longjmp (spu_indirect_branch): unwind the
         * leaked coroutine/poll frames and re-dispatch from ctx->pc on a fresh
         * stack. SPU state is in the heap ctx, so it survives the unwind. */
        (void)setjmp(restart_jb);
        g_spu_trampoline_fn = 0;
        sctx->host_depth = 0;        /* the longjmp destroyed every lifted call
                                      * frame: re-sync SPU_RET's depth counter
                                      * so unbalanced `bi $r0` returns dispatch
                                      * to r0 instead of C-returning here */
        spu_indirect_branch(sctx);   /* runs until stop; halt longjmps back */
        SPU_DRAIN(sctx);             /* iterate tail-call chains (scheduler loop) */
    }
    g_spu_halt_jmp = 0;
    g_spu_restart_jmp = 0;
    g_spu_stack_base = 0;
    g_spu_trampoline_fn = 0;

    fprintf(stderr, "[SPU] tid=0x%X stopped (status=0x%X pc=0x%05X code=0x%X)\n",
            t->tid, sctx->status, sctx->pc, sctx->stop_code);
    fflush(stderr);

    /* F18: surface the real SPU exit status instead of hardcoding 0. The SPU-side
     * sys_spu_thread_exit ABI writes the status to SPU_WrOutMbox, then executes
     * stop 0x102 (THREAD_EXIT); lv2 pops that mailbox value as the exit status.
     * The 14-bit stop code (sctx->stop_code) is only the dispatch selector, NOT
     * the status itself (CBEA p97 SPU_Status.StopCode). 0x102 THREAD_EXIT ->
     * this thread's status; 0x101 GROUP_EXIT -> the group's status. specaudit F18. */
    if (sctx->status == SPU_STATUS_STOPPED_BY_STOP &&
        sctx->stop_code == 0x102u /* THREAD_EXIT */ && sctx->ch_out_mbox.count) {
        t->exit_status = (int32_t)sctx->ch_out_mbox.value;   /* guest-written status */
        sctx->ch_out_mbox.count = 0;
    } else if (sctx->status == SPU_STATUS_STOPPED_BY_STOP &&
               sctx->stop_code == 0x101u /* GROUP_EXIT */ && sctx->ch_out_mbox.count) {
        spu_group_t* grp = spu_find_group(t->group_id);
        if (grp) grp->exit_status = (int32_t)sctx->ch_out_mbox.value;
        sctx->ch_out_mbox.count = 0;
        t->exit_status = 0;
    } else {
        t->exit_status = 0;   /* no exit-protocol stop: unchanged from prior behavior */
    }
#ifdef _WIN32
    t->running = 0;
    SetEvent(t->finish_event);
    return 0;
#else
    pthread_mutex_lock(&t->finish_event.mu);
    t->running = 0;
    t->finish_event.done = 1;
    pthread_cond_broadcast(&t->finish_event.cv);
    pthread_mutex_unlock(&t->finish_event.mu);
    return NULL;
#endif
}

/* Host-thread entry for a PPU-fallback SPU thread. */
#ifdef _WIN32
static DWORD WINAPI spu_fallback_thread_proc(LPVOID arg)
#else
static void* spu_fallback_thread_proc(void* arg)
#endif
{
    spu_thread_t* t = (spu_thread_t*)arg;
    int32_t rc = 0;
    if (t->fb_handler) {
        rc = t->fb_handler(t->tid, t->args_ea, t->args_size, t->fb_user);
    }
    t->exit_status = rc;
    /* Mark complete and signal anyone waiting in group_join. */
#ifdef _WIN32
    t->running = 0;
    SetEvent(t->finish_event);
    return 0;
#else
    pthread_mutex_lock(&t->finish_event.mu);
    t->running = 0;
    t->finish_event.done = 1;
    pthread_cond_broadcast(&t->finish_event.cv);
    pthread_mutex_unlock(&t->finish_event.mu);
    return NULL;
#endif
}

/* sys_spu_thread_group_start(id) */
static int64_t sys_spu_thread_group_start_handler(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(id);
    if (!g) { ctx->gpr[3] = (uint64_t)(int64_t)-1; return -1; }
    g->state = SPU_GROUP_STATE_RUNNING;

    /* For each thread in the group, look up a registered PPU fallback by
     * the thread's SPU image entry point. Threads with a fallback run on
     * a host thread (real concurrency, like real SPUs). Threads without
     * a fallback complete instantly with status 0.
     * group_join() blocks until all spawned host threads finish. */
    int spawned = 0;
    int instant = 0;
    for (uint32_t i = 0; i < g->num_threads && i < 8; i++) {
        uint32_t idx = g->thread_indices[i];
        if (idx >= MAX_SPU_THREADS) continue;
        spu_thread_t* t = &s_spu_threads[idx];
        if (!t->in_use) continue;

        /* Real SPU execution first: if a lifted image covers the entry
         * point, run the actual SPU code on a host thread. */
        if (spu_have_function(t->entry_point)) {
            if (!t->sctx) t->sctx = (spu_context*)calloc(1, sizeof(spu_context));
            if (t->sctx) {
                spu_context* sctx = t->sctx;
                memset(sctx, 0, sizeof(*sctx));
                sctx->spu_id       = t->tid;
                sctx->spu_group_id = id;
                if (t->img_ea) spu_deploy_image(sctx, t->img_ea);
                /* args captured at thread_initialize -> gpr[3..6]
                 * preferred DW (lane 0 = high word, lane 1 = low word) */
                for (int a = 0; a < 4; a++) {
                    sctx->gpr[3 + a]._u32[0] = (uint32_t)(t->args[a] >> 32);
                    sctx->gpr[3 + a]._u32[1] = (uint32_t)t->args[a];
                }
                sctx->pc = t->entry_point & SPU_LS_MASK;
                t->running = 1;
#ifdef _WIN32
                if (!t->finish_event)
                    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
                else
                    ResetEvent(t->finish_event);
                /* 64 MB stack: lifted SPU code tail-calls between
                 * functions; give the chain room if the compiler keeps
                 * the frames. */
                t->host_thread = CreateThread(NULL, 64 * 1024 * 1024,
                                              spu_exec_thread_proc, t,
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
#else
                pthread_mutex_init(&t->finish_event.mu, NULL);
                pthread_cond_init(&t->finish_event.cv, NULL);
                t->finish_event.done = 0;
                pthread_create(&t->host_thread, NULL, spu_exec_thread_proc, t);
#endif
                fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X "
                        "args=0x%08X -> REAL SPU execution "
                        "(arg1=0x%08X%08X arg2=0x%08X%08X arg3=0x%08X%08X)\n",
                        id, t->tid, t->entry_point, t->args_ea,
                        sctx->gpr[3]._u32[0], sctx->gpr[3]._u32[1],
                        sctx->gpr[4]._u32[0], sctx->gpr[4]._u32[1],
                        sctx->gpr[5]._u32[0], sctx->gpr[5]._u32[1]);
                spawned++;
                continue;
            }
        }

        void* user = NULL;
        spu_ppu_fallback_fn fb = spu_lookup_ppu_fallback(t->entry_point, &user);
        if (!fb) {
            t->exit_status = 0;
            t->running = 0;
            instant++;
            continue;
        }
        t->fb_handler = fb;
        t->fb_user    = user;
        t->running    = 1;
#ifdef _WIN32
        /* Manual-reset event so multiple group_join callers all see "set" */
        if (!t->finish_event)
            t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        else
            ResetEvent(t->finish_event);
        t->host_thread = CreateThread(NULL, 0, spu_fallback_thread_proc, t, 0, NULL);
#else
        pthread_mutex_init(&t->finish_event.mu, NULL);
        pthread_cond_init(&t->finish_event.cv, NULL);
        t->finish_event.done = 0;
        pthread_create(&t->host_thread, NULL, spu_fallback_thread_proc, t);
#endif
        fprintf(stderr, "[SPU] group_start id=0x%X tid=0x%X entry=0x%08X args=0x%08X -> spawned host thread\n",
                id, t->tid, t->entry_point, t->args_ea);
        spawned++;
    }

    if (spawned == 0) {
        g->state = SPU_GROUP_STATE_STOPPED;
        g->cause = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
        g->exit_status = 0;
        fprintf(stderr, "[SPU] group_start id=0x%X (no fallback for any of %u thread(s); instantly completed)\n",
                id, g->num_threads);
    } else {
        fprintf(stderr, "[SPU] group_start id=0x%X (%d host threads running, %d instant)\n",
                id, spawned, instant);
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

    /* Wait for any host-thread fallbacks to finish, then collect the
     * worst exit status. Real SPU group_join is a blocking syscall —
     * games rely on it to know all SPU work is done before reading
     * back results. */
    if (g->state == SPU_GROUP_STATE_RUNNING) {
        int32_t worst = 0;
        for (int i = 0; i < 8 && i < (int)g->num_threads; i++) {
            uint32_t idx = g->thread_indices[i];
            if (idx >= MAX_SPU_THREADS) continue;
            spu_thread_t* t = &s_spu_threads[idx];
            if (!t->in_use) continue;
            if (t->running) {
#ifdef _WIN32
                if (t->finish_event)
                    WaitForSingleObject(t->finish_event, INFINITE);
                if (t->host_thread) {
                    CloseHandle(t->host_thread);
                    t->host_thread = NULL;
                }
#else
                pthread_mutex_lock(&t->finish_event.mu);
                while (!t->finish_event.done)
                    pthread_cond_wait(&t->finish_event.cv, &t->finish_event.mu);
                pthread_mutex_unlock(&t->finish_event.mu);
                pthread_join(t->host_thread, NULL);
                pthread_mutex_destroy(&t->finish_event.mu);
                pthread_cond_destroy(&t->finish_event.cv);
#endif
            }
            if (t->exit_status < worst) worst = t->exit_status;
        }
        g->exit_status = worst;
        g->cause       = SPU_GROUP_CAUSE_ALL_THREADS_EXIT;
        g->state       = SPU_GROUP_STATE_STOPPED;
    }

    /* Notify any connected event queue. Real PS3 sends a SYS_SPU_THREAD_GROUP
     * event with type-specific data; we collapse to a "group stopped" tag
     * (data1 = group_id, data2 = exit_status, data3 = cause). PPU code
     * blocked in sys_event_queue_receive on this queue wakes up here.
     *
     * Audit sec.5 item 5 (2026-07-03, user-confirmed): the event `source`
     * must be the real SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY, not the raw
     * group_id -- RPCS3 sys_spu.h:42/311 + sys_spu.cpp:1266
     * (group->send_run_event() on group start/stop) sends via `ep_run`
     * with source = SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY
     * (0xFFFFFFFF53505500, sys_spu.h:42). Receivers that switch on source
     * (rather than trusting whichever queue fired) got the bare group_id,
     * which collides with the guest's own generic tag namespace. SPURS
     * uses the (correct) user-key path instead of this group-event path,
     * so the practical impact is low -- flagged low priority in the audit. */
    if (g->event_queue_id) {
        sys_event_queue_push_by_id(g->event_queue_id,
                                   0xFFFFFFFF53505500ull, /* SYS_SPU_THREAD_GROUP_EVENT_RUN_KEY */
                                   (uint64_t)(int64_t)g->exit_status,
                                   (uint64_t)g->cause,
                                   0);
    }

    vm_write_be32(cause_ea,  g->cause);
    vm_write_be32(status_ea, (uint32_t)g->exit_status);

    fprintf(stderr, "[SPU] group_join id=0x%X cause=%u status=%d (event_queue=0x%X)\n",
            id, g->cause, g->exit_status, g->event_queue_id);
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
            if (idx < MAX_SPU_THREADS) {
                spu_thread_t* t = &s_spu_threads[idx];
                if (t->local_store) {
                    free(t->local_store);
                    t->local_store = NULL;
                }
                t->in_use = 0;
            }
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

/* sys_spu_thread_get_exit_status(tid, *status)
 * Real PS3: returns CELL_ESRCH for unknown tid, CELL_ESTAT if thread is
 * still running (caller should join the group first), otherwise 0 with
 * the exit code written through. */
static int64_t sys_spu_thread_get_exit_status_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t status_ea = (uint32_t)ctx->gpr[4];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (t->running) {
        /* Still in flight — Sony's behaviour. Games that want the exit code
         * synchronously should call group_join first. */
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000F; /* CELL_ESTAT (was 0x80010003 = ENOSYS) */
        return -1;
    }
    vm_write_be32(status_ea, (uint32_t)t->exit_status);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_set_argument(tid, arg_ea) — doesn't affect us, log only */
static int64_t sys_spu_thread_set_argument_handler(ppu_context* ctx)
{
    uint32_t tid    = (uint32_t)ctx->gpr[3];
    uint32_t arg_ea = (uint32_t)ctx->gpr[4];

    /* Update the per-thread args pointer so any registered PPU fallback
     * picks it up at sys_spu_thread_group_start time. The shape of the
     * struct at arg_ea is whatever the game registered for — typically
     * a packed (arg1,arg2,arg3,arg4) tuple of 4 u64s on real SPUs. */
    spu_thread_t* t = spu_find_thread(tid);
    if (t) {
        t->args_ea = arg_ea;
        for (int a = 0; a < 4; a++) {     /* lv2 copy semantics */
            uint64_t hi = arg_ea ? vm_read_be32(arg_ea + (uint32_t)a * 8)     : 0;
            uint64_t lo = arg_ea ? vm_read_be32(arg_ea + (uint32_t)a * 8 + 4) : 0;
            t->args[a] = (hi << 32) | lo;
        }
    }

    fprintf(stderr, "[SPU] thread_set_argument tid=0x%X arg=0x%08X\n",
            tid, arg_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_connect_event(group_id, queue_id, event_type)
 * sys_spu_thread_group_connect_event_all_threads(group_id, queue_id, name, port)
 *
 * Both bind a SYS_EVENT queue to the group; we record queue_id so
 * group_join can push a completion event. Sony's docs distinguish event
 * types (group state changes vs SPU-emitted user events) but we collapse
 * them into "the queue gets notified when the group transitions to
 * STOPPED" — sufficient for the common SPURS pattern. */
static int64_t sys_spu_thread_group_connect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    uint32_t queue_id = (uint32_t)ctx->gpr[4];
    spu_group_t* g = spu_find_group(group_id);
    if (!g) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    g->event_queue_id = queue_id;
    fprintf(stderr, "[SPU] group_connect_event group=0x%X queue=0x%X\n",
            group_id, queue_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_group_connect_event_all_threads(group_id, queue_id, req, spup)
 * (syscall 251). req = bitmask of allowed SPU ports; allocate the first free port
 * in req, bind the queue to it (per group), and WRITE the chosen port number to the
 * *spup out-param (u8). The caller (libsre/libmixer) uses that port to identify
 * SPU-emitted class-2 events. Our prior handler ignored req + never wrote *spup, so
 * the caller got a garbage port -> SPU<->PPU event routing was broken -> libmixer's
 * setup never completed (no cellAudioPortStart) and the CRI player stalled.
 * Matches RPCS3 sys_spu.cpp:sys_spu_thread_group_connect_event_all_threads. */
static int64_t sys_spu_thread_group_connect_event_all_threads_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    uint32_t queue_id = (uint32_t)ctx->gpr[4];
    uint64_t req      = ctx->gpr[5];
    uint32_t spup_ea  = (uint32_t)ctx->gpr[6];   /* out: u8 assigned port */
    spu_group_t* g = spu_find_group(group_id);
    if (!g)   { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; return -1; } /* ESRCH  */
    if (!req) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; return -1; } /* EINVAL */
    int port = -1;
    for (int p = 0; p < 64; p++) {
        if (!(req & (1ull << p))) continue;
        if (g->spup_queue[p] == 0) { port = p; break; }
    }
    if (port < 0) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010015; return -1; } /* CELL_EISCONN (was 0x8001000C = EABORT) */
    g->spup_queue[port] = queue_id;
    if (spup_ea) vm_write8(spup_ea, (uint8_t)port);   /* the out-param the caller reads */
    fprintf(stderr, "[SPU] group_connect_event_all_threads group=0x%X queue=0x%X "
            "req=0x%llX -> port=%d (spup@0x%X)\n", group_id, queue_id,
            (unsigned long long)req, port, spup_ea);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

static int64_t sys_spu_thread_group_disconnect_event_handler(ppu_context* ctx)
{
    uint32_t group_id = (uint32_t)ctx->gpr[3];
    spu_group_t* g = spu_find_group(group_id);
    if (g) g->event_queue_id = 0;
    fprintf(stderr, "[SPU] group_disconnect_event group=0x%X\n", group_id);
    fflush(stderr);
    ctx->gpr[3] = 0;
    return 0;
}

/* SPU virtual local store. Real hardware: 256 KB per SPU. We allocate on
 * first read/write so the common case (group with no LS access) doesn't
 * waste 256 KB × num_threads. */
#define SPU_LS_SIZE  (256 * 1024)
static uint8_t* spu_thread_get_or_alloc_ls(spu_thread_t* t)
{
    if (!t) return NULL;
    if (!t->local_store) {
        t->local_store = (uint8_t*)calloc(1, SPU_LS_SIZE);
    }
    return t->local_store;
}

/* sys_spu_thread_write_ls(tid, ls_offset, value, type)
 * Writes 1/2/4/8 bytes (per `type`: 1/2/4/8) into the SPU thread's LS
 * at ls_offset. Real PS3 sees this stored to the SPU's local memory; we
 * keep an independent per-thread buffer that the PPU and any registered
 * fallback can access via spu_thread_get_local_store(). */
static int64_t sys_spu_thread_write_ls_handler(ppu_context* ctx)
{
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t ls_offset = (uint32_t)ctx->gpr[4];
    uint64_t value     = (uint64_t)ctx->gpr[5];
    uint32_t type      = (uint32_t)ctx->gpr[6];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (ls_offset + type > SPU_LS_SIZE) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000D; /* CELL_EFAULT (was 0x80010002 = EINVAL) */
        return -1;
    }
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004; /* CELL_ENOMEM */
        return -1;
    }
    /* Big-endian store, mirroring guest convention. */
    switch (type) {
    case 1: ls[ls_offset] = (uint8_t)value; break;
    case 2: ls[ls_offset+0] = (uint8_t)(value >> 8);
            ls[ls_offset+1] = (uint8_t)value; break;
    case 4: for (int i = 0; i < 4; i++)
                ls[ls_offset+i] = (uint8_t)(value >> ((3-i)*8));
            break;
    case 8: for (int i = 0; i < 8; i++)
                ls[ls_offset+i] = (uint8_t)(value >> ((7-i)*8));
            break;
    default:
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_read_ls(tid, ls_offset, *value_out, type) */
static int64_t sys_spu_thread_read_ls_handler(ppu_context* ctx)
{
    extern uint8_t* vm_base;
    uint32_t tid       = (uint32_t)ctx->gpr[3];
    uint32_t ls_offset = (uint32_t)ctx->gpr[4];
    uint32_t value_ea  = (uint32_t)ctx->gpr[5];
    uint32_t type      = (uint32_t)ctx->gpr[6];
    spu_thread_t* t = spu_find_thread(tid);
    if (!t || !value_ea || !vm_base) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (ls_offset + type > SPU_LS_SIZE) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000D; /* CELL_EFAULT (was 0x80010002 = EINVAL) */
        return -1;
    }
    uint8_t* ls = spu_thread_get_or_alloc_ls(t);
    if (!ls) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010004;
        return -1;
    }
    /* Big-endian load → write to guest as 8 bytes (always); the syscall
     * is documented to write a u64 with the value zero-extended in the
     * high bits. */
    uint64_t value = 0;
    switch (type) {
    case 1: value = ls[ls_offset]; break;
    case 2: value = ((uint64_t)ls[ls_offset] << 8) | ls[ls_offset+1]; break;
    case 4:
        for (int i = 0; i < 4; i++)
            value = (value << 8) | ls[ls_offset+i];
        break;
    case 8:
        for (int i = 0; i < 8; i++)
            value = (value << 8) | ls[ls_offset+i];
        break;
    default:
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002;
        return -1;
    }
    /* Write 8-byte BE value to guest. */
    uint8_t* p = vm_base + value_ea;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(value >> ((7-i)*8));
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_write_snr (sc-184): tid, number (0/1), value.
 * Writes an SPU Signal Notification Register. CBE Registers v1.5
 * (SPU_Sig_Notify_1/2): in OR mode (SPU_Cfg bit 0/1, sc-187) the value is
 * ORed into the pending register; in overwrite mode (default) it replaces
 * it. The SPU reads via RdSigNotify1/2 (read-and-clear; channel count = 1
 * while a value is pending). Was a silent no-op stub until 2026-07-03
 * (CBEA audit P8) -- every lv2-side signal was dropped.
 * NOTE: the SNR-write SPU event edge (CBEA event facility S1/S2 bits) is
 * not raised here -- consistent with the rest of the channel model (audit
 * F10, its own queued fix). */
static int64_t sys_spu_thread_write_snr_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint32_t num = (uint32_t)ctx->gpr[4];
    uint32_t val = (uint32_t)ctx->gpr[5];
    if (num > 1) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; /* CELL_EINVAL */
        return -1;
    }
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    if (t->sctx) {
        spu_channel* ch = &t->sctx->ch_sig_notify[num];
        int or_mode = (t->spu_cfg >> num) & 1;
        if (or_mode && ch->count)
            ch->value |= val;
        else
            spu_channel_write(ch, val);
        { static int n = 0; if (n < 12) { n++;
            fprintf(stderr, "[SPU] write_snr tid=0x%X snr%u <- 0x%08X (%s)\n",
                    tid, num + 1, val, or_mode ? "OR" : "overwrite");
            fflush(stderr); } }
    }
    /* No live SPU context (thread never started as lifted code): accept and
     * drop, like real lv2 writing a problem-state register of a stopped SPU. */
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_spu_thread_set_spu_cfg (sc-187): tid, value (bits 0-1 = SNR1/SNR2 OR
 * mode). Was entirely absent from the syscall table (CBEA audit P8). */
static int64_t sys_spu_thread_set_spu_cfg_handler(ppu_context* ctx)
{
    uint32_t tid = (uint32_t)ctx->gpr[3];
    uint64_t val = (uint64_t)ctx->gpr[4];
    if (val & ~3ull) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010002; /* CELL_EINVAL */
        return -1;
    }
    spu_thread_t* t = spu_find_thread(tid);
    if (!t) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80010005; /* CELL_ESRCH */
        return -1;
    }
    t->spu_cfg = val;
    ctx->gpr[3] = 0;
    return 0;
}

/* Public: get the local-store buffer for a SPU thread (for use by
 * PPU-fallback handlers). Allocates on demand. */
uint8_t* spu_thread_get_local_store(uint32_t tid)
{
    return spu_thread_get_or_alloc_ls(spu_find_thread(tid));
}

uint32_t spu_thread_local_store_size(void) { return SPU_LS_SIZE; }

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

/* sys_process_getpid (1) — lv2 returns 1 for the game process (RPCS3
 * sys_process.cpp: "sys_process_getpid() -> 1"; the real-HW trace shows
 * libsre calling get_sdk_version with pid=0x1). */
static int64_t sys_process_getpid_handler(ppu_context* ctx)
{
    ctx->gpr[3] = 1;
    return 1;
}

/* sys_process_get_sdk_version (25): (pid, u32* version). Writes the
 * process's SDK version from PROC_PARAM (offset 0xC); the loader sets
 * g_ps3_sdk_version. Yakuza: Dead Souls = 0x350001 (RPCS3 ppu_loader). */
uint32_t g_ps3_sdk_version = 0x00350001;

static int64_t sys_process_get_sdk_version_handler(ppu_context* ctx)
{
    uint32_t version_ea = (uint32_t)ctx->gpr[4];
    if (!version_ea) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EFAULT;
        return (int64_t)(int32_t)CELL_EFAULT;
    }
    vm_write_be32(version_ea, g_ps3_sdk_version);
    ctx->gpr[3] = 0;
    return 0;
}

/* sys_process_is_spu_lock_line_reservation_address (14)
 *
 * r3 = addr, r4 = access-right flags (SPU_THR = 0x2, RAW_SPU = 0x1).
 * Asks lv2 whether SPUs may place lock-line reservations (GETLLAR/PUTLLC)
 * on the given effective address. SPURS calls this while validating its
 * management areas during initialization; a failure aborts SPURS init.
 *
 * Contract from RPCS3 sys_process.cpp process_is_spu_lock_line_reservation
 * _address(), mapped onto OUR guest layout: flags must be non-zero and
 * contain only the two SPU bits (else EINVAL); main memory, the sys_memory
 * window and RSX local memory are reservation-capable (OK); PPU stacks and
 * sys_vm regions are not (EPERM); anything unmapped is EINVAL. */
static int64_t sys_process_is_spu_lock_line_reservation_address(ppu_context* ctx)
{
    uint32_t addr  = (uint32_t)ctx->gpr[3];
    uint64_t flags = ctx->gpr[4];
    int64_t  rc;

    if (!flags || (flags & ~0x3ull)) {
        rc = (int64_t)(int32_t)CELL_EINVAL;
    } else if (addr >= VM_MAIN_MEM_BASE && addr < VM_MAIN_MEM_BASE + VM_MAIN_MEM_SIZE) {
        rc = 0;                                   /* main memory */
    } else if (addr >= 0x40000000u && addr < 0x50000000u) {
        rc = 0;                                   /* sys_memory window */
    } else if (addr >= 0xC0000000u && addr < 0xD0000000u) {
        rc = 0;                                   /* RSX local memory */
    } else if (addr >= 0xD0000000u && addr < 0xE0000000u) {
        rc = (int64_t)(int32_t)CELL_EPERM;        /* PPU stack area */
    } else if (addr >= SYS_VM_REGION_BASE && addr < SYS_VM_REGION_END) {
        rc = (int64_t)(int32_t)CELL_EPERM;        /* sys_vm memory */
    } else {
        rc = (int64_t)(int32_t)CELL_EINVAL;       /* unmapped */
    }

    ctx->gpr[3] = (uint64_t)rc;
    return rc;
}

/* ---------------------------------------------------------------------------
 * lwmutex / lwcond KERNEL SLOW-PATH syscalls (sc 95-99, 111-117)
 *
 * Audit sec.6 (2026-07-03, user-confirmed): these numbers were defined in
 * lv2_syscall_table.h but never registered -- a lw primitive that needs to
 * fall through to the kernel (contended lock beyond the userspace fast
 * path, or a lwcond wait) would have hit the unimplemented-syscall handler.
 * MEASURED latent: 0 hits so far, because the actual guest entry point for
 * this game is sysPrxForUser::sys_lwmutex_* / sys_lwcond_* (a library
 * function bridge, import_bridges_gen.cpp), which is HLE'd in full in
 * libs/system/sysPrxForUser.c and does its own userspace fast-path +
 * blocking without ever executing the raw `sc N` instructions below. These
 * exist for completeness/robustness (any code path that DOES issue the raw
 * syscall, e.g. a differently-linked SDK routine) and are independent
 * kernel objects distinct from that HLE's tables.
 *
 * Faithful subset of the RPCS3 contract (sys_lwmutex.cpp / sys_lwcond.cpp):
 * create validates protocol; destroy refuses EBUSY while held; lock/trylock/
 * unlock implement ordinary (non-fast-path) mutex semantics; lwcond wait
 * atomically releases the associated lwmutex and reacquires it on wake,
 * mirroring sys_cond_wait's now-corrected recursive-release handling
 * (audit item 1c) since a lwmutex can equally be SYS_SYNC_RECURSIVE
 * (has no separate "unlock2"/targeted-signal fast paths -- those are
 * userspace-only optimizations with no guest-visible kernel-object state
 * here). Signal/signal_all simply wake waiters; RPCS3's targeted
 * ppu_thread_id signal (_sys_lwcond_signal's 3rd arg) is honored on a
 * best-effort basis by ID equality since we don't have RPCS3's PPU sleep
 * queue -- broadcasting a superset of the intended wake target is safe
 * (spurious wakes retry their condition, same as sys_cond).
 * -----------------------------------------------------------------------*/

#define MAX_LW_KERNEL_MUTEX  256
#define MAX_LW_KERNEL_COND   256

typedef struct {
    int      in_use;
    uint32_t protocol;
    int      recursive;      /* SYS_SYNC_RECURSIVE control->attribute bit (best-effort: caller-tracked) */
    uint64_t owner_tid;
    int      lock_count;
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} lw_kernel_mutex_t;

typedef struct {
    int      in_use;
    uint32_t lwmutex_id;     /* 1-based index into s_lw_mutexes */
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cv;
#endif
} lw_kernel_cond_t;

static lw_kernel_mutex_t s_lw_mutexes[MAX_LW_KERNEL_MUTEX];
static lw_kernel_cond_t  s_lw_conds[MAX_LW_KERNEL_COND];

#ifdef _WIN32
static CRITICAL_SECTION s_lw_table_lock;
static int              s_lw_table_lock_init = 0;
#else
static pthread_mutex_t  s_lw_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void lw_table_lock(void)
{
#ifdef _WIN32
    if (!s_lw_table_lock_init) {
        InitializeCriticalSection(&s_lw_table_lock);
        s_lw_table_lock_init = 1;
    }
    EnterCriticalSection(&s_lw_table_lock);
#else
    pthread_mutex_lock(&s_lw_table_lock);
#endif
}

static void lw_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_lw_table_lock);
#else
    pthread_mutex_unlock(&s_lw_table_lock);
#endif
}

/* _sys_lwmutex_create(lwmutex_id*, protocol, control_ptr, has_name, name) */
static int64_t sys_lwmutex_create_syscall(ppu_context* ctx)
{
    uint32_t id_out_ea = (uint32_t)ctx->gpr[3];
    uint32_t protocol  = (uint32_t)ctx->gpr[4];
    /* control_ptr (gpr[5]), has_name (gpr[6]), name (gpr[7]) -- unused: the
     * kernel object doesn't touch the guest control struct (userspace owns
     * the fast-path fields; see file header). */

    if (protocol != SYS_SYNC_FIFO && protocol != SYS_SYNC_RETRY && protocol != SYS_SYNC_PRIORITY) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EINVAL;
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    lw_table_lock();
    int slot = -1;
    for (int i = 0; i < MAX_LW_KERNEL_MUTEX; i++) {
        if (!s_lw_mutexes[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        lw_table_unlock();
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EAGAIN;
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    lw_kernel_mutex_t* m = &s_lw_mutexes[slot];
    memset(m, 0, sizeof(*m));
    m->in_use   = 1;
    m->protocol = protocol;
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mtx, &mattr);
    pthread_mutexattr_destroy(&mattr);
#endif
    lw_table_unlock();

    vm_write_be32(id_out_ea, (uint32_t)(slot + 1));
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwmutex_destroy(lwmutex_id) */
static int64_t sys_lwmutex_destroy_syscall(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    if (id == 0 || id > MAX_LW_KERNEL_MUTEX) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    lw_kernel_mutex_t* m = &s_lw_mutexes[id - 1];
    if (!m->in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (m->lock_count != 0) {
        /* RPCS3 sys_lwmutex.cpp: refuses to destroy a held lwmutex. */
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EBUSY;
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#ifdef _WIN32
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->mtx);
#endif
    m->in_use = 0;
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwmutex_lock(lwmutex_id, timeout_usec) */
static int64_t sys_lwmutex_lock_syscall(ppu_context* ctx)
{
    uint32_t id      = (uint32_t)ctx->gpr[3];
    uint64_t timeout = ctx->gpr[4];
    if (id == 0 || id > MAX_LW_KERNEL_MUTEX) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    lw_kernel_mutex_t* m = &s_lw_mutexes[id - 1];
    if (!m->in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    uint64_t caller_tid = ctx->thread_id;
#ifdef _WIN32
    if (timeout == 0) {
        EnterCriticalSection(&m->cs);
    } else {
        /* Timed lock to a QPC deadline (same fix as sys_mutex_lock: the old
         * GetTickCount/Sleep(1) loop overshot sub-ms timeouts by the ~15.6 ms
         * timer tick). */
        int64_t deadline = lv2_usec_deadline(timeout);
        while (!TryEnterCriticalSection(&m->cs)) {
            if (lv2_deadline_passed(deadline)) {
                ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ETIMEDOUT;
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
            if (timeout < 1000) SwitchToThread();
            else                Sleep(1);
        }
    }
#else
    if (timeout == 0) {
        pthread_mutex_lock(&m->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout / 1000000);
        ts.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_mutex_timedlock(&m->mtx, &ts) != 0) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ETIMEDOUT;
            return (int64_t)(int32_t)CELL_ETIMEDOUT;
        }
    }
#endif
    m->owner_tid = caller_tid;
    m->lock_count++;
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwmutex_trylock(lwmutex_id) */
static int64_t sys_lwmutex_trylock_syscall(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    if (id == 0 || id > MAX_LW_KERNEL_MUTEX) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    lw_kernel_mutex_t* m = &s_lw_mutexes[id - 1];
    if (!m->in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
#ifdef _WIN32
    if (!TryEnterCriticalSection(&m->cs)) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EBUSY;
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#else
    if (pthread_mutex_trylock(&m->mtx) != 0) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EBUSY;
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#endif
    m->owner_tid = ctx->thread_id;
    m->lock_count++;
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwmutex_unlock(lwmutex_id) */
static int64_t sys_lwmutex_unlock_syscall(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    if (id == 0 || id > MAX_LW_KERNEL_MUTEX) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    lw_kernel_mutex_t* m = &s_lw_mutexes[id - 1];
    if (!m->in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (m->owner_tid != ctx->thread_id) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EPERM;
        return (int64_t)(int32_t)CELL_EPERM;
    }
    m->lock_count--;
    if (m->lock_count == 0) m->owner_tid = 0;
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwcond_create(lwcond_id*, lwmutex_id, control_ptr, name) */
static int64_t sys_lwcond_create_syscall(ppu_context* ctx)
{
    uint32_t id_out_ea = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex_id = (uint32_t)ctx->gpr[4];

    if (lwmutex_id == 0 || lwmutex_id > MAX_LW_KERNEL_MUTEX || !s_lw_mutexes[lwmutex_id - 1].in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    lw_table_lock();
    int slot = -1;
    for (int i = 0; i < MAX_LW_KERNEL_COND; i++) {
        if (!s_lw_conds[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        lw_table_unlock();
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EAGAIN;
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    lw_kernel_cond_t* c = &s_lw_conds[slot];
    memset(c, 0, sizeof(*c));
    c->in_use     = 1;
    c->lwmutex_id = lwmutex_id;
#ifdef _WIN32
    InitializeConditionVariable(&c->cv);
#else
    pthread_cond_init(&c->cv, NULL);
#endif
    lw_table_unlock();

    vm_write_be32(id_out_ea, (uint32_t)(slot + 1));
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwcond_destroy(lwcond_id) */
static int64_t sys_lwcond_destroy_syscall(ppu_context* ctx)
{
    uint32_t id = (uint32_t)ctx->gpr[3];
    if (id == 0 || id > MAX_LW_KERNEL_COND || !s_lw_conds[id - 1].in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
#ifndef _WIN32
    pthread_cond_destroy(&s_lw_conds[id - 1].cv);
#endif
    s_lw_conds[id - 1].in_use = 0;
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwcond_queue_wait(lwcond_id, lwmutex_id, timeout_usec) -- syscall 113.
 * Mirrors sys_cond_wait's corrected recursive-release handling (audit item
 * 1c): release ALL of the caller's recursion levels on the associated
 * lwmutex before parking, restore the saved count on wake. */
static int64_t sys_lwcond_wait_syscall(ppu_context* ctx)
{
    uint32_t cond_id    = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex_id = (uint32_t)ctx->gpr[4];
    uint64_t timeout    = ctx->gpr[5];

    if (cond_id == 0 || cond_id > MAX_LW_KERNEL_COND || !s_lw_conds[cond_id - 1].in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    lw_kernel_cond_t* c = &s_lw_conds[cond_id - 1];
    if (lwmutex_id == 0 || lwmutex_id > MAX_LW_KERNEL_MUTEX || !s_lw_mutexes[lwmutex_id - 1].in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    lw_kernel_mutex_t* m = &s_lw_mutexes[lwmutex_id - 1];

    if (m->owner_tid != ctx->thread_id) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_EPERM;
        return (int64_t)(int32_t)CELL_EPERM;
    }

    uint64_t saved_owner = m->owner_tid;
    int saved_count = m->lock_count;
    m->owner_tid = 0;
    m->lock_count = 0;

#ifdef _WIN32
    DWORD ms = (timeout == 0) ? INFINITE : (DWORD)(timeout / 1000);
    if (ms == 0 && timeout > 0) ms = 1;

    for (int i = 1; i < saved_count; i++) LeaveCriticalSection(&m->cs);
    BOOL ok = SleepConditionVariableCS(&c->cv, &m->cs, ms);
    for (int i = 1; i < saved_count; i++) EnterCriticalSection(&m->cs);

    m->owner_tid = saved_owner;
    m->lock_count = saved_count;

    if (!ok && GetLastError() == ERROR_TIMEOUT) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ETIMEDOUT;
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
#else
    for (int i = 1; i < saved_count; i++) pthread_mutex_unlock(&m->mtx);

    int rc;
    if (timeout == 0) {
        rc = pthread_cond_wait(&c->cv, &m->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout / 1000000);
        ts.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        rc = pthread_cond_timedwait(&c->cv, &m->mtx, &ts);
    }

    for (int i = 1; i < saved_count; i++) pthread_mutex_lock(&m->mtx);

    m->owner_tid = saved_owner;
    m->lock_count = saved_count;

    if (rc == ETIMEDOUT) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ETIMEDOUT;
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
#endif

    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwcond_signal(lwcond_id, lwmutex_id, ppu_thread_id, mode) -- syscall
 * 115. We don't have RPCS3's PPU sleep queue to target one specific thread
 * id, so this broadcasts like signal_all; every waiter re-checks its own
 * condition on wake (same tolerance as a spurious wakeup), which is safe. */
static int64_t sys_lwcond_signal_syscall(ppu_context* ctx)
{
    uint32_t cond_id = (uint32_t)ctx->gpr[3];
    if (cond_id == 0 || cond_id > MAX_LW_KERNEL_COND || !s_lw_conds[cond_id - 1].in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
#ifdef _WIN32
    WakeAllConditionVariable(&s_lw_conds[cond_id - 1].cv);
#else
    pthread_cond_broadcast(&s_lw_conds[cond_id - 1].cv);
#endif
    ctx->gpr[3] = 0;
    return 0;
}

/* _sys_lwcond_signal_all(lwcond_id, lwmutex_id, mode) -- syscall 116 */
static int64_t sys_lwcond_signal_all_syscall(ppu_context* ctx)
{
    uint32_t cond_id = (uint32_t)ctx->gpr[3];
    if (cond_id == 0 || cond_id > MAX_LW_KERNEL_COND || !s_lw_conds[cond_id - 1].in_use) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ESRCH;
        return (int64_t)(int32_t)CELL_ESRCH;
    }
#ifdef _WIN32
    WakeAllConditionVariable(&s_lw_conds[cond_id - 1].cv);
#else
    pthread_cond_broadcast(&s_lw_conds[cond_id - 1].cv);
#endif
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

    /* lwmutex/lwcond KERNEL SLOW-PATH (audit sec.6, 2026-07-03,
     * user-confirmed): previously defined in lv2_syscall_table.h but never
     * registered -- see the handler block above for why they're latent
     * (the game's actual entry point is the sysPrxForUser HLE library,
     * not these raw syscalls) and the faithful-subset contract notes. */
    lv2_syscall_register(tbl, SYS_LWMUTEX_CREATE,   sys_lwmutex_create_syscall);
    lv2_syscall_register(tbl, SYS_LWMUTEX_DESTROY,  sys_lwmutex_destroy_syscall);
    lv2_syscall_register(tbl, SYS_LWMUTEX_LOCK,     sys_lwmutex_lock_syscall);
    lv2_syscall_register(tbl, SYS_LWMUTEX_UNLOCK,   sys_lwmutex_unlock_syscall);
    lv2_syscall_register(tbl, SYS_LWMUTEX_TRYLOCK,  sys_lwmutex_trylock_syscall);
    lv2_syscall_register(tbl, SYS_LWCOND_CREATE,     sys_lwcond_create_syscall);
    lv2_syscall_register(tbl, SYS_LWCOND_DESTROY,    sys_lwcond_destroy_syscall);
    lv2_syscall_register(tbl, SYS_LWCOND_WAIT,       sys_lwcond_wait_syscall);
    lv2_syscall_register(tbl, SYS_LWCOND_SIGNAL,     sys_lwcond_signal_syscall);
    lv2_syscall_register(tbl, SYS_LWCOND_SIGNAL_ALL, sys_lwcond_signal_all_syscall);

    /* Timer and time (registered before events so event handlers
     * override the conflicting syscall numbers 141, 142, 145) */
    sys_timer_init(tbl);

    /* Event queues, ports, and flags */
    sys_event_init(tbl);

    /* Memory management */
    sys_memory_init(tbl);
    sys_vm_init(tbl);

    /* Filesystem */
    sys_fs_init(tbl);

    /* Process queries */
    lv2_syscall_register(tbl, 1,  sys_process_getpid_handler);
    lv2_syscall_register(tbl, 25, sys_process_get_sdk_version_handler);
    lv2_syscall_register(tbl, 14, sys_process_is_spu_lock_line_reservation_address);

    /* TTY (debug console I/O — used by CRT startup) */
    lv2_syscall_register(tbl, SYS_TTY_READ,  sys_tty_read);
    lv2_syscall_register(tbl, SYS_TTY_WRITE, sys_tty_write);

    /* SPU syscalls — we don't execute SPU code but the PPU-side wrappers
     * need consistent IDs and out-params. See the stateful group tracker
     * above for contract notes. */
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
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT, sys_spu_thread_group_connect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT, sys_spu_thread_group_disconnect_event_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_LS,        sys_spu_thread_write_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_READ_LS,         sys_spu_thread_read_ls_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_WRITE_SNR,       sys_spu_thread_write_snr_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_SET_SPU_CFG,     sys_spu_thread_set_spu_cfg_handler);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_BIND_QUEUE,      sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_UNBIND_QUEUE,    sys_spu_thread_stub);
    lv2_syscall_register(tbl, SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS, sys_spu_thread_group_connect_event_all_threads_handler);
}
