/*
 * Firmware-free, host-native SPURS scheduler.
 *
 * The implementation deliberately keeps guest object bytes authoritative.
 * Host locks, condition variables and workers are held in bounded side tables
 * keyed by the guest object pointer.
 */
#include "cellSpurs.h"
#include "../../runtime/spu/spu_workload.h"
#include "../../runtime/spu/spu_job_dispatch.h"
#include "../../runtime/memory/vm.h"
#if !defined(YZ_SPURS_TEST_GUEST_SIZE)
#include "../../runtime/syscalls/sys_vm.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION nmutex;
typedef CONDITION_VARIABLE ncond;
typedef HANDLE nthread;
#define NTHREAD_INVALID NULL
#define NATIVE_SPU_HOST_STACK_SIZE (64u * 1024u * 1024u)
static void mx_init(nmutex* m) { InitializeCriticalSection(m); }
static void mx_destroy(nmutex* m) { DeleteCriticalSection(m); }
static void mx_lock(nmutex* m) { EnterCriticalSection(m); }
static void mx_unlock(nmutex* m) { LeaveCriticalSection(m); }
static void cv_init(ncond* c) { InitializeConditionVariable(c); }
static void cv_wait(ncond* c, nmutex* m) { SleepConditionVariableCS(c, m, INFINITE); }
static void cv_wake_all(ncond* c) { WakeAllConditionVariable(c); }
static void cv_wake_one(ncond* c) { WakeConditionVariable(c); }
static void nthread_yield(void) { SwitchToThread(); }
static void nthread_reschedule(void) { Sleep(1); }
static int nthread_create_spu(nthread* thread, LPTHREAD_START_ROUTINE entry, void* opaque)
{
    /*
     * Lifted SPU functions retain host call frames.  Use the same reservation
     * as the LLE SPU executor so task and job workers can run those functions
     * without exhausting the platform's small default thread stack.
     */
    *thread = CreateThread(NULL, NATIVE_SPU_HOST_STACK_SIZE, entry, opaque,
                           STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    return *thread != NULL;
}
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_mutex_t nmutex;
typedef pthread_cond_t ncond;
typedef pthread_t nthread;
#define NTHREAD_INVALID ((pthread_t)0)
#define NATIVE_SPU_HOST_STACK_SIZE (64u * 1024u * 1024u)
static void mx_init(nmutex* m) { pthread_mutex_init(m, NULL); }
static void mx_destroy(nmutex* m) { pthread_mutex_destroy(m); }
static void mx_lock(nmutex* m) { pthread_mutex_lock(m); }
static void mx_unlock(nmutex* m) { pthread_mutex_unlock(m); }
static void cv_init(ncond* c) { pthread_cond_init(c, NULL); }
static void cv_wait(ncond* c, nmutex* m) { pthread_cond_wait(c, m); }
static void cv_wake_all(ncond* c) { pthread_cond_broadcast(c); }
static void cv_wake_one(ncond* c) { pthread_cond_signal(c); }
static void nthread_yield(void) { sched_yield(); }
static void nthread_reschedule(void)
{
    const struct timespec interval = {0, 1000000};
    nanosleep(&interval, NULL);
}
static int nthread_create_spu(nthread* thread, void* (*entry)(void*), void* opaque)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return 0;
    const int stack_rc = pthread_attr_setstacksize(&attr, NATIVE_SPU_HOST_STACK_SIZE);
    const int create_rc = stack_rc ? stack_rc : pthread_create(thread, &attr, entry, opaque);
    pthread_attr_destroy(&attr);
    return create_rc == 0;
}
#endif

static void native_spu_context_free(spu_context* ctx)
{
    if (!ctx) return;
    spu_mfc_unregister(ctx);
    free(ctx);
}

static u16 rd16(const void* v)
{
    const u8* p = (const u8*)v;
    return (u16)(((u16)p[0] << 8) | p[1]);
}
static u32 rd32(const void* v)
{
    const u8* p = (const u8*)v;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static u64 rd64(const void* v) { return ((u64)rd32(v) << 32) | rd32((const u8*)v + 4); }
static void wr16(void* v, u16 x)
{
    u8* p = (u8*)v; p[0] = (u8)(x >> 8); p[1] = (u8)x;
}
static void wr32(void* v, u32 x)
{
    u8* p = (u8*)v;
    p[0] = (u8)(x >> 24); p[1] = (u8)(x >> 16); p[2] = (u8)(x >> 8); p[3] = (u8)x;
}
static void wr64(void* v, u64 x) { wr32(v, (u32)(x >> 32)); wr32((u8*)v + 4, (u32)x); }
static u32 guest_ea(const void* p)
{
    if (!p) return 0;
    if (vm_base && (const u8*)p >= vm_base)
        return (u32)((const u8*)p - vm_base);
    return (u32)(uintptr_t)p;
}
static int job_guest_range_in_window(u32 ea, u64 size, u32 base, u64 bytes)
{
    const u64 end = (u64)ea + size;
    const u64 limit = (u64)base + bytes;
    if (end > 0x100000000ull || limit > 0x100000000ull)
        return 0;
    if (size == 0)
        return (u64)ea >= base && (u64)ea <= limit;
    return (u64)ea >= base && end <= limit;
}
static int job_guest_range_in_fixed_main_storage(u32 ea, u64 size)
{
    /* These are PPU-addressable storage windows, not SPU local store or RSX
     * local memory.  sys_memory commits its complete window during runtime
     * initialization, so an EA there is safe to inspect even before the
     * title's bump allocator has formally handed out the containing block. */
    return job_guest_range_in_window(
               ea, size, VM_MAIN_MEM_BASE, VM_MAIN_MEM_SIZE) ||
           job_guest_range_in_window(
               ea, size, VM_STACK_BASE, VM_STACK_REGION) ||
           job_guest_range_in_window(
               ea, size, 0x40000000u, 0x10000000ull) ||
           job_guest_range_in_window(
               ea, size, 0x50000000u, 0x00400000ull);
}
#if defined(YZ_SPURS_TEST_GUEST_SIZE)
int cellSpursTestProductionMainStorageRange(u32 ea, u64 size)
{
    return job_guest_range_in_fixed_main_storage(ea, size);
}
#endif
static int job_guest_range_valid(u32 ea, u64 size)
{
    if (!vm_base) return 0;
    const u64 end = (u64)ea + size;
    if (end > 0x100000000ull) return 0;
#if defined(YZ_SPURS_TEST_GUEST_SIZE)
    return end <= (u64)YZ_SPURS_TEST_GUEST_SIZE;
#else
    if (job_guest_range_in_fixed_main_storage(ea, size))
        return 1;

    /* sys_vm commits only each live mapping, so use its allocation records
     * instead of accepting the complete reserved address window. */
    for (int i = 0; i < SYS_VM_MAX; ++i) {
        const sys_vm_map_info* mapping = &g_sys_vm_maps[i];
        if (mapping->active && job_guest_range_in_window(
                ea, size, mapping->addr, mapping->vsize))
            return 1;
    }
    return 0;
#endif
}
static int aligned(const void* p, uintptr_t n) { return ((uintptr_t)p & (n - 1)) == 0; }
static void diag(const char* kind, const void* object, u64 value)
{
    fprintf(stderr, "[native-spurs] divergence=%s object=0x%08X value=0x%llX\n",
            kind, guest_ea(object), (unsigned long long)value);
}
static int native_trace_enabled(void)
{
    static int initialized, enabled;
    if (!initialized) {
        enabled = getenv("YZ_NATIVE_SPURS_TRACE") != NULL;
        initialized = 1;
    }
    return enabled;
}

static int lfqueue_trace_enabled(void)
{
    static int initialized, enabled;
    if (!initialized) {
        enabled = getenv("YZ_NATIVE_SPURS_LFQUEUE_TRACE") != NULL;
        initialized = 1;
    }
    return enabled;
}

static int jobchain_ledger_enabled(void)
{
    static int initialized, enabled;
    if (!initialized) {
        enabled = getenv("YZ_NATIVE_SPURS_LEDGER") != NULL;
        initialized = 1;
    }
    return enabled;
}

/* ------------------------------------------------------------------------- */
/* Side-table synchronization registry                                       */

#define MAX_SPURS_INSTANCES 16
#define MAX_TASKSETS 64
#define MAX_EVENT_FLAGS 128
#define MAX_QUEUES 128
#define MAX_JOBCHAINS 32
#define MAX_JOB_GUARDS 64

typedef struct {
    void* key;
    nmutex mutex;
    ncond cond;
    int live;
} SyncKey;

static nmutex g_registry_mutex;
static volatile int g_registry_ready;

#if defined(_WIN32)
static void registry_init(void)
{
    if (g_registry_ready == 2) return;
    if (InterlockedCompareExchange((volatile LONG*)&g_registry_ready, 1, 0) == 0) {
        mx_init(&g_registry_mutex);
        InterlockedExchange((volatile LONG*)&g_registry_ready, 2);
    } else {
        while (g_registry_ready != 2) SwitchToThread();
    }
}
#else
static void registry_init_once(void)
{
    mx_init(&g_registry_mutex);
    g_registry_ready = 2;
}
static void registry_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, registry_init_once);
}
#endif

static void sync_init(SyncKey* s, void* key)
{
    memset(s, 0, sizeof(*s));
    s->key = key;
    mx_init(&s->mutex);
    cv_init(&s->cond);
    s->live = 1;
}

/* ------------------------------------------------------------------------- */
/* SPURS instances and attributes                                            */

typedef struct {
    SyncKey sync;
    u32 nspus;
    u32 next_wid;
    u32 active_wkl_mask;
    u32 runnable_wkl_mask;
    u32 next_spu_num;
    int shutdown;
} SpursState;
static SpursState g_spurs[MAX_SPURS_INSTANCES];

static int spurs_allocate_workload(SpursState* state, u32* out_wid)
{
    if (!state || !out_wid) return 0;
    mx_lock(&state->sync.mutex);
    for (u32 offset = 0; offset < 32; ++offset) {
        const u32 wid = (state->next_wid + offset) & 31u;
        const u32 mask = 0x80000000u >> wid;
        if (!(state->active_wkl_mask & mask)) {
            state->active_wkl_mask |= mask;
            state->next_wid = (wid + 1u) & 31u;
            *out_wid = wid;
            mx_unlock(&state->sync.mutex);
            return 1;
        }
    }
    mx_unlock(&state->sync.mutex);
    return 0;
}

static void spurs_release_workload(SpursState* state, u32 wid)
{
    if (!state || wid >= 32) return;
    const u32 mask = 0x80000000u >> wid;
    mx_lock(&state->sync.mutex);
    state->active_wkl_mask &= ~mask;
    state->runnable_wkl_mask &= ~mask;
    mx_unlock(&state->sync.mutex);
}

static SpursState* spurs_find(const void* key)
{
    for (u32 i = 0; i < MAX_SPURS_INSTANCES; ++i)
        if (g_spurs[i].sync.live && g_spurs[i].sync.key == key) return &g_spurs[i];
    return NULL;
}
static SpursState* spurs_make(void* key)
{
    registry_init();
    mx_lock(&g_registry_mutex);
    SpursState* result = spurs_find(key);
    if (!result) {
        for (u32 i = 0; i < MAX_SPURS_INSTANCES; ++i) {
            if (!g_spurs[i].sync.live) {
                result = &g_spurs[i];
                sync_init(&result->sync, key);
                break;
            }
        }
    }
    mx_unlock(&g_registry_mutex);
    return result;
}

s32 _cellSpursAttributeInitialize(CellSpursAttribute* a, u32 rev, u32 sdk,
                                  u32 nspus, s32 spu_prio, s32 ppu_prio, u8 exit_no_work)
{
    if (!a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (!aligned(a, 8)) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (!nspus || nspus > 8) return CELL_SPURS_CORE_ERROR_INVAL;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes + 0x00, rev);
    wr32(a->bytes + 0x04, sdk);
    wr32(a->bytes + 0x08, nspus);
    wr32(a->bytes + 0x0c, (u32)spu_prio);
    wr32(a->bytes + 0x10, (u32)ppu_prio);
    a->bytes[0x14] = exit_no_work;
    return CELL_OK;
}
s32 cellSpursAttributeInitialize(CellSpursAttribute* a, s32 n, s32 sp, s32 pp, u8 ex)
{
    return _cellSpursAttributeInitialize(a, 2, 0x475001, (u32)n, sp, pp, ex);
}
s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* a, const char* name, u32 size)
{
    if (!a || !name) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (size > 15) return CELL_SPURS_CORE_ERROR_INVAL;
    memcpy(a->bytes + 0x20, name, size);
    a->bytes[0x20 + size] = 0;
    a->bytes[0x30] = (u8)size;
    return CELL_OK;
}
s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* a, s32 type)
{
    if (!a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    wr32(a->bytes + 0x18, (u32)type);
    return CELL_OK;
}
s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* a)
{
    if (!a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    a->bytes[0x1c] = 1;
    return CELL_OK;
}
static s32 spurs_initialize(void* object, size_t size, const CellSpursAttribute* a)
{
    if (!object || !a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (!aligned(object, 128)) return CELL_SPURS_CORE_ERROR_ALIGN;
    SpursState* state = spurs_make(object);
    if (!state) return CELL_SPURS_CORE_ERROR_NOMEM;
    memset(object, 0, size);
    state->nspus = rd32(a->bytes + 0x08);
    /* Ordinary workloads occupy IDs 0..31.  The scheduler's system service
     * uses its separate sentinel ID and does not consume an ordinary slot. */
    state->next_wid = 0;
    state->active_wkl_mask = 0;
    state->runnable_wkl_mask = 0;
    state->next_spu_num = 0;
    state->shutdown = 0;
    spu_workload_set_image_executor(spu_native_image_executor);
    ((u8*)object)[0x76] = (u8)state->nspus;
    fprintf(stderr, "[native-spurs] initialize ea=0x%08X spus=%u\n",
            guest_ea(object), state->nspus);
    return CELL_OK;
}
s32 cellSpursInitializeWithAttribute(CellSpurs* s, const CellSpursAttribute* a)
{
    return spurs_initialize(s, sizeof(*s), a);
}
s32 cellSpursInitializeWithAttribute2(CellSpurs2* s, const CellSpursAttribute* a)
{
    return spurs_initialize(s, sizeof(*s), a);
}
s32 cellSpursInitialize(CellSpurs* s, s32 n, s32 sp, s32 pp, u8 ex)
{
    CellSpursAttribute a;
    s32 rc = cellSpursAttributeInitialize(&a, n, sp, pp, ex);
    return rc ? rc : cellSpursInitializeWithAttribute(s, &a);
}
s32 cellSpursFinalize(CellSpurs* s)
{
    if (!s) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    SpursState* state = spurs_find(s);
    if (!state) return CELL_SPURS_CORE_ERROR_STAT;
    mx_lock(&state->sync.mutex);
    state->shutdown = 1;
    cv_wake_all(&state->sync.cond);
    mx_unlock(&state->sync.mutex);
    return CELL_OK;
}

/* ------------------------------------------------------------------------- */
/* Tasksets and task execution                                               */

typedef struct TasksetState TasksetState;
typedef struct {
    TasksetState* owner;
    u32 id;
    u32 spu_num;
    const u8* elf;
    u32 elf_size;
    u32 context_ea;
    u32 context_size;
    u8 ls_pattern[16];
    u8 argument[16];
    spu_workload_image image;
    nthread thread;
    int thread_valid;
    int complete;
    int joined;
    int reaping;
    int waiting;
    int signalled;
    int exit_code;
    unsigned idle_poll_count;
    unsigned trace_syscalls;
    unsigned profile_poll_workload_hits;
    unsigned profile_yield_calls;
} TaskState;

struct TasksetState {
    SyncKey sync;
    void* spurs;
    u64 args;
    u32 size;
    u32 wid;
    int wid_registered;
    u32 max_contention;
    u8 priority[8];
    int shutdown;
    TaskState tasks[128];
};
static TasksetState g_tasksets[MAX_TASKSETS];

static TasksetState* taskset_find(const void* key)
{
    for (u32 i = 0; i < MAX_TASKSETS; ++i)
        if (g_tasksets[i].sync.live && g_tasksets[i].sync.key == key) return &g_tasksets[i];
    return NULL;
}
static TasksetState* taskset_make(void* key)
{
    registry_init();
    mx_lock(&g_registry_mutex);
    TasksetState* result = taskset_find(key);
    if (!result) {
        for (u32 i = 0; i < MAX_TASKSETS; ++i) {
            if (!g_tasksets[i].sync.live) {
                result = &g_tasksets[i];
                memset(result, 0, sizeof(*result));
                sync_init(&result->sync, key);
                break;
            }
        }
    }
    mx_unlock(&g_registry_mutex);
    return result;
}
static void task_bit(void* ts, u32 off, u32 id, int value)
{
    u8* p = (u8*)ts + off + id / 8;
    u8 mask = (u8)(0x80u >> (id & 7));
    if (value) *p |= mask; else *p &= (u8)~mask;
}
static void spurs_set_workload_runnable(void* object, u32 wid, int runnable)
{
    SpursState* spurs = spurs_find(object);
    if (!spurs || wid >= 32) return;
    const u32 mask = 0x80000000u >> wid;
    mx_lock(&spurs->sync.mutex);
    if (runnable)
        spurs->runnable_wkl_mask |= mask;
    else
        spurs->runnable_wkl_mask &= ~mask;
    mx_unlock(&spurs->sync.mutex);
}
static int taskset_has_ready_locked(const TasksetState* ts)
{
    if (ts->shutdown) return 0;
    const u8* object = (const u8*)ts->sync.key;
    for (u32 byte = 0; byte < CELL_SPURS_MAX_TASK / 8; ++byte)
        if (object[0x10 + byte] & (u8)~object[0x00 + byte]) return 1;
    return 0;
}
static void taskset_refresh_runnable_locked(TasksetState* ts)
{
    spurs_set_workload_runnable(
        ts->spurs, ts->wid, taskset_has_ready_locked(ts));
}
static u32 ls_pattern_blocks(const u8 pattern[16])
{
    u32 total = 0;
    for (u32 i = 0; i < 16; ++i) {
        u8 x = pattern[i];
        for (u32 b = 0; b < 8; ++b) total += (x >> b) & 1;
    }
    return total;
}
static void task_publish(TaskState* t)
{
    u8* ts = (u8*)t->owner->sync.key;
    u8* info = ts + 0x80 + t->id * 0x30;
    memcpy(info + 0x00, t->argument, 16);
    wr64(info + 0x10, guest_ea(t->elf));
    wr64(info + 0x18, ((u64)t->context_ea & ~0x7full) | ls_pattern_blocks(t->ls_pattern));
    memcpy(info + 0x20, t->ls_pattern, 16);
    task_bit(ts, 0x30, t->id, 1); /* enabled */
    task_bit(ts, 0x10, t->id, 1); /* ready */
}
static u128 task_load_qword(const u8* bytes)
{
    u128 value;
    for (u32 lane = 0; lane < 4; ++lane)
        value._u32[lane] = rd32(bytes + lane * 4);
    return value;
}
static void task_store_qword(u8* bytes, const u128* value)
{
    for (u32 lane = 0; lane < 4; ++lane)
        wr32(bytes + lane * 4, value->_u32[lane]);
}
static void task_save_context(TaskState* t, spu_context* ctx)
{
    if (!t->context_ea || t->context_size < 1024 || !vm_base) return;
    u8* out = vm_base + t->context_ea;
    memset(out, 0, 1024);
    task_store_qword(out + 0x000, &ctx->gpr[0]);
    task_store_qword(out + 0x010, &ctx->gpr[1]);
    for (u32 reg = 80; reg < 128; ++reg)
        task_store_qword(out + 0x020 + (reg - 80) * 16, &ctx->gpr[reg]);
    task_store_qword(out + 0x320, &ctx->fpscr);
    wr32(out + 0x334, ctx->event_mask);
    u32 cursor = 1024;
    for (u32 block = 0; block < 128 && cursor + 2048 <= t->context_size; ++block) {
        if (t->ls_pattern[block / 8] & (0x80u >> (block & 7))) {
            memcpy(out + cursor, ctx->ls + block * 2048, 2048);
            cursor += 2048;
        }
    }
}
static void task_restore_context(TaskState* t, spu_context* ctx)
{
    if (!t->context_ea || t->context_size < 1024 || !vm_base) return;
    const u8* in = vm_base + t->context_ea;
    ctx->gpr[0] = task_load_qword(in + 0x000);
    ctx->gpr[1] = task_load_qword(in + 0x010);
    for (u32 reg = 80; reg < 128; ++reg)
        ctx->gpr[reg] = task_load_qword(in + 0x020 + (reg - 80) * 16);
    ctx->fpscr = task_load_qword(in + 0x320);
    ctx->event_mask = rd32(in + 0x334);
    u32 cursor = 1024;
    for (u32 block = 0; block < 128 && cursor + 2048 <= t->context_size; ++block) {
        if (t->ls_pattern[block / 8] & (0x80u >> (block & 7))) {
            memcpy(ctx->ls + block * 2048, in + cursor, 2048);
            cursor += 2048;
        }
    }
}
static int task_syscall(spu_context* ctx, void* opaque)
{
    TaskState* t = (TaskState*)opaque;
    TasksetState* ts = t->owner;
    const u32 op = ctx->gpr[3]._u32[0] & 0x0f;
    if (native_trace_enabled()) {
        const unsigned n = ++t->trace_syscalls;
        if (n <= 64 || (n & 0xfffffu) == 0)
            fprintf(stderr,
                    "[native-spurs-trace] task-syscall n=%u task=%u "
                    "op=%u pc=0x%05X lr=0x%05X sp=0x%05X "
                    "r4=%08X_%08X_%08X_%08X r5=%08X "
                    "ts{run=%08X ready=%08X pend=%08X en=%08X sig=%08X wait=%08X}\n",
                    n, t->id, op, ctx->pc,
                    ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    ctx->gpr[1]._u32[0] & SPU_LS_MASK,
                    ctx->gpr[4]._u32[0], ctx->gpr[4]._u32[1],
                    ctx->gpr[4]._u32[2], ctx->gpr[4]._u32[3],
                    ctx->gpr[5]._u32[0],
                    rd32(ctx->ls + 0x2700), rd32(ctx->ls + 0x2710),
                    rd32(ctx->ls + 0x2720), rd32(ctx->ls + 0x2730),
                    rd32(ctx->ls + 0x2740), rd32(ctx->ls + 0x2750));
    }
    if (op == 0) {
        /* The task exit wrapper saves its exit code in the taskset context
         * before it reuses r4 to locate the syscall entry.  Reading r4 here
         * reports that internal LS address (usually 0x27c4), not the value
         * supplied to cellSpursExit. */
        t->exit_code = (s32)rd32(ctx->ls + 0x2fd0);
        return -1;
    }
    if (op == 1 || op == 2) {
        /* Both scheduler handoffs save LS/context for another dispatch.
         * Complete all task DMA first so WAIT_SIGNAL cannot expose a partial
         * context any more than YIELD can. */
        spu_task_wait_all_dma(ctx);
        task_save_context(t, ctx);
        spu_mfc_context_switch(ctx);
        mx_lock(&ts->sync.mutex);
        if (op == 1) {
            /* YIELD returns the task to READY while its host worker gives the
             * scheduler a chance to run another native workload. */
            task_bit(ts->sync.key, 0x00, t->id, 0);
            taskset_refresh_runnable_locked(ts);
            mx_unlock(&ts->sync.mutex);
#if defined(YZ_PERF_PROFILE)
            if (++t->profile_yield_calls == 1)
                fprintf(stderr,
                        "[native-spurs-profile] task=%u wid=%u first-yield\n",
                        t->id, ts->wid);
#endif
            /* A task yield is a scheduler transition, not merely a CPU hint.
             * Give the producer and peer workloads one bounded host interval
             * before this task is selected again. */
            nthread_reschedule();
            mx_lock(&ts->sync.mutex);
            if (!ts->shutdown) {
                task_bit(ts->sync.key, 0x00, t->id, 1);
                taskset_refresh_runnable_locked(ts);
            }
        } else {
            t->waiting = 1;
            task_bit(ts->sync.key, 0x00, t->id, 0);
            task_bit(ts->sync.key, 0x10, t->id, 0);
            task_bit(ts->sync.key, 0x50, t->id, 1);
            taskset_refresh_runnable_locked(ts);
            while (!t->signalled && !ts->shutdown)
                cv_wait(&ts->sync.cond, &ts->sync.mutex);
            t->signalled = 0;
            task_bit(ts->sync.key, 0x40, t->id, 0);
            task_bit(ts->sync.key, 0x50, t->id, 0);
            if (!ts->shutdown) {
                task_bit(ts->sync.key, 0x00, t->id, 1);
                task_bit(ts->sync.key, 0x10, t->id, 1);
            }
            t->waiting = 0;
            taskset_refresh_runnable_locked(ts);
        }
        mx_unlock(&ts->sync.mutex);
        task_restore_context(t, ctx);
        ctx->gpr[3] = spu_make_preferred_u32(ts->shutdown ?
                                             (u32)CELL_SPURS_TASK_ERROR_SHUTDOWN : 0);
        return ts->shutdown ? 0 : 1;
    }
    if (op == 3) {
        spu_task_wait_all_dma(ctx);
        const u8* object = (const u8*)ts->sync.key;
        int found_task = 0;
        int found_workload = 0;
        mx_lock(&ts->sync.mutex);
        for (u32 id = 0; id < CELL_SPURS_MAX_TASK; ++id) {
            const u8 mask = (u8)(0x80u >> (id & 7));
            const u32 byte = id / 8;
            if (id != t->id && (object[0x10 + byte] & mask) &&
                !(object[0x00 + byte] & mask)) {
                found_task = 1;
                break;
            }
        }
        mx_unlock(&ts->sync.mutex);
        SpursState* spurs = spurs_find(ts->spurs);
        if (spurs) {
            const u32 own = ts->wid < 32 ? 0x80000000u >> ts->wid : 0;
            mx_lock(&spurs->sync.mutex);
            found_workload = (spurs->runnable_wkl_mask & ~own) != 0;
            mx_unlock(&spurs->sync.mutex);
        }
        if (found_task || found_workload)
            t->idle_poll_count = 0;
        else
            ++t->idle_poll_count;
        const u32 result = (found_task ? 1u : 0u) |
                           (found_workload ? 2u : 0u);
        if (!result)
            nthread_yield();
        if (native_trace_enabled() &&
            (t->trace_syscalls <= 320u || result != 0)) {
            fprintf(stderr,
                    "[native-spurs-trace] task-poll-return n=%u task=%u "
                    "result=%u task-ready=%d workload-yield=%d idle=%u\n",
                    t->trace_syscalls, t->id, result, found_task,
                    found_workload, t->idle_poll_count);
        }
#if defined(YZ_PERF_PROFILE)
        if (found_workload && ++t->profile_poll_workload_hits == 1)
            fprintf(stderr,
                    "[native-spurs-profile] task=%u wid=%u "
                    "task-poll-found-workload result=%u\n",
                    t->id, ts->wid, result);
#endif
        ctx->gpr[3] = spu_make_preferred_u32(result);
        return 1;
    }
    if (op == 4) {
        /* Workload-flag receipt is distinct from a task signal.  The native
         * backend does not currently expose workload registration/receiver
         * imports, so accepting a task signal here would silently violate the
         * ABI.  Surface the unsupported syscall deterministically instead. */
        ctx->gpr[3] = spu_make_preferred_u32((u32)CELL_SPURS_TASK_ERROR_NOSYS);
        diag("task-workload-flag-unsupported", ts->sync.key, t->id);
        return 1;
    }
    diag("task-syscall", ts->sync.key, op);
    return 0;
}

static void task_run(TaskState* task)
{
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
    fprintf(stderr,
            "[native-spurs-profile] task=%u wid=%u image=%d host_tid=%lu\n",
            task->id, task->owner->wid, task->image.image_id,
            GetCurrentThreadId());
#endif
    spu_context* ctx = (spu_context*)malloc(sizeof(*ctx));
    if (!ctx) {
        task->exit_code = CELL_SPURS_TASK_ERROR_NOMEM;
        goto finished;
    }
    spu_context_init(ctx, task->spu_num);
    u32 entry = 0;
    if (!spu_elf_load_to_ls(task->elf, task->elf_size, ctx->ls, &entry)) {
        task->exit_code = CELL_SPURS_TASK_ERROR_NOEXEC;
        native_spu_context_free(ctx);
        goto finished;
    }

    /* Task-side SPURS libraries inspect the resident kernel context even when
     * the scheduler itself is host-native.  Populate the public ABI fields the
     * taskset policy would have left at LS 0x100; no firmware image is loaded
     * or executed. */
    SpursState* spurs = spurs_find(task->owner->spurs);
    u32 active_wkl_mask = 0;
    if (spurs) {
        mx_lock(&spurs->sync.mutex);
        active_wkl_mask = spurs->active_wkl_mask;
        mx_unlock(&spurs->sync.mutex);
    }
    if (task->owner->wid < 16)
        ctx->ls[0x1a0 + task->owner->wid] =
            task->owner->priority[task->spu_num & 7u];
    wr64(ctx->ls + 0x1c0, guest_ea(task->owner->spurs));
    wr32(ctx->ls + 0x1c8, task->spu_num);
    wr32(ctx->ls + 0x1cc, 31); /* CELL_SPURS_KERNEL_DMA_TAG_ID */
    wr32(ctx->ls + 0x1dc, task->owner->wid);
    wr32(ctx->ls + 0x1e0, 0x0838); /* task exit-to-scheduler ABI target */
    wr32(ctx->ls + 0x1e4, 0x0290); /* workload-selection ABI target */
    wr16(ctx->ls + 0x1e8, 0x544b); /* SPURS task module id: "TK" */
    ctx->ls[0x1ea] = 1;
    wr16(ctx->ls + 0x1ec, (u16)(active_wkl_mask >> 16));
    wr16(ctx->ls + 0x1ee, (u16)active_wkl_mask);

    /* The taskset policy adds running before it snapshots the 128-byte header.
     * Ready remains set while a task is runnable; it is cleared by WAIT_SIGNAL,
     * not by selection. */
    mx_lock(&task->owner->sync.mutex);
    task_bit(task->owner->sync.key, 0x00, task->id, 1);
    taskset_refresh_runnable_locked(task->owner);
    mx_unlock(&task->owner->sync.mutex);

    u8* stc = ctx->ls + 0x2700;
    memcpy(stc, task->owner->sync.key, 128);
    memcpy(ctx->ls + 0x2780, task->argument, 16);
    wr64(ctx->ls + 0x2790, guest_ea(task->elf));
    wr64(ctx->ls + 0x2798,
         ((u64)task->context_ea & ~0x7full) |
             ls_pattern_blocks(task->ls_pattern));
    memcpy(ctx->ls + 0x27a0, task->ls_pattern, 16);
    wr64(ctx->ls + 0x27b8, guest_ea(task->owner->sync.key));
    wr32(ctx->ls + 0x27c0, 0x0100); /* SpursKernelContext LS address */
    wr32(ctx->ls + 0x27c4, 0x0a70);
    wr32(ctx->ls + 0x27cc, task->spu_num);
    wr32(ctx->ls + 0x27d0, 31);     /* CELL_SPURS_KERNEL_DMA_TAG_ID */
    wr32(ctx->ls + 0x27d4, task->id);
    memcpy(ctx->ls + 0x2840, "SPURSTASK MODULE", 16);
    wr32(ctx->ls + 0x2fb8, 0x2700);
    wr32(ctx->ls + 0x2fbc, 0x3000);
    ctx->gpr[3] = task_load_qword(ctx->ls + 0x2780);
    {
        const u128 taskset_data = task_load_qword(ctx->ls + 0x2760);
        ctx->gpr[4]._u32[0] = taskset_data._u32[2];
        ctx->gpr[4]._u32[1] = taskset_data._u32[3];
        ctx->gpr[4]._u32[2] = taskset_data._u32[0];
        ctx->gpr[4]._u32[3] = taskset_data._u32[1];
    }
    ctx->gpr[1] = spu_make_preferred_u32(0x2c30);
    ctx->native_spurs_syscall = task_syscall;
    ctx->native_spurs_opaque = task;
    ctx->pc = task->image.entry_pc ? task->image.entry_pc : entry;
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] task-start task=%u image=%d "
                "entry=0x%05X elf=0x%08X size=0x%X "
                "r3=%08X_%08X_%08X_%08X r4=%08X_%08X_%08X_%08X\n",
                task->id, task->image.image_id, ctx->pc,
                guest_ea(task->elf), task->elf_size,
                ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                ctx->gpr[4]._u32[0], ctx->gpr[4]._u32[1],
                ctx->gpr[4]._u32[2], ctx->gpr[4]._u32[3]);

    if (getenv("YZ_NATIVE_TASK_BOOTSTRAP_WAIT") && task->image.name &&
        strcmp(task->image.name, "gs_task") == 0 && vm_base) {
        const u32 bootstrap_ea = rd32(task->argument);
        unsigned waits = 0;
        if (bootstrap_ea) {
            while (rd32(vm_base + bootstrap_ea) == 0) {
                ++waits;
                nthread_yield();
            }
            atomic_thread_fence(memory_order_acquire);
            fprintf(stderr,
                    "[native-spurs-bootstrap] task=%u image=%s ea=0x%08X "
                    "first=0x%08X waits=%u\n",
                    task->id, task->image.name, bootstrap_ea,
                    rd32(vm_base + bootstrap_ea), waits);
        }
    }

    if (!spu_workload_execute(&task->image, ctx))
        task->exit_code = CELL_SPURS_TASK_ERROR_NOEXEC;
    native_spu_context_free(ctx);
finished:
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] task-finish task=%u rc=0x%08X\n",
                task->id, (u32)task->exit_code);
    mx_lock(&task->owner->sync.mutex);
    task_bit(task->owner->sync.key, 0x00, task->id, 0);
    task_bit(task->owner->sync.key, 0x10, task->id, 0);
    task_bit(task->owner->sync.key, 0x30, task->id, 0);
    task_bit(task->owner->sync.key, 0x40, task->id, 0);
    task_bit(task->owner->sync.key, 0x50, task->id, 0);
    task->complete = 1;
    taskset_refresh_runnable_locked(task->owner);
    cv_wake_all(&task->owner->sync.cond);
    mx_unlock(&task->owner->sync.mutex);
}
#if defined(_WIN32)
static DWORD WINAPI task_thread_proc(LPVOID p) { task_run((TaskState*)p); return 0; }
#else
static void* task_thread_proc(void* p) { task_run((TaskState*)p); return NULL; }
#endif
static int task_start_thread(TaskState* t)
{
    t->thread_valid = nthread_create_spu(&t->thread, task_thread_proc, t);
    return t->thread_valid;
}
static void task_join_thread(TasksetState* ts, TaskState* t)
{
    nthread thread;
    mx_lock(&ts->sync.mutex);
    while (t->reaping)
        cv_wait(&ts->sync.cond, &ts->sync.mutex);
    if (!t->thread_valid || t->joined) {
        mx_unlock(&ts->sync.mutex);
        return;
    }
    t->reaping = 1;
    thread = t->thread;
    mx_unlock(&ts->sync.mutex);
#if defined(_WIN32)
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
    mx_lock(&ts->sync.mutex);
    t->joined = 1;
    t->thread_valid = 0;
    t->reaping = 0;
    cv_wake_all(&ts->sync.cond);
    mx_unlock(&ts->sync.mutex);
}

s32 _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* a, u32 rev,
                                         u32 sdk, u64 args, const u8* prio, u32 max)
{
    if (!a) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes + 0x00, rev);
    wr32(a->bytes + 0x04, sdk);
    wr64(a->bytes + 0x08, args);
    if (prio) memcpy(a->bytes + 0x10, prio, 8);
    wr32(a->bytes + 0x18, max);
    wr32(a->bytes + 0x20, CELL_SPURS_TASKSET_SIZE);
    return CELL_OK;
}
void _cellSpursTasksetAttribute2Initialize(CellSpursTasksetAttribute2* a, u32 rev)
{
    if (!a) return;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes, rev);
    memset(a->bytes + 0x10, 1, 8);
    wr32(a->bytes + 0x18, 8);
}
s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* a)
{
    u8 p[8] = {1,1,1,1,1,1,1,1};
    return _cellSpursTasksetAttributeInitialize(a, 1, 0x475001, 0, p, 8);
}
s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute* a, const char* name)
{
    if (!a || !name) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    wr32(a->bytes + 0x1c, guest_ea(name));
    return CELL_OK;
}
s32 cellSpursTasksetAttributeSetTasksetSize(CellSpursTasksetAttribute* a, size_t size)
{
    if (!a) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (size < CELL_SPURS_TASKSET_SIZE) return CELL_SPURS_TASK_ERROR_INVAL;
    wr32(a->bytes + 0x20, (u32)size);
    return CELL_OK;
}
static s32 create_taskset_common(CellSpurs* spurs, void* taskset, u32 size,
                                  u64 args, const u8* priority,
                                  u32 max_contention, u8 enable_clear_ls)
{
    if (!spurs || !taskset) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(taskset, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SpursState* spurs_state = spurs_find(spurs);
    if (!spurs_state) return CELL_SPURS_TASK_ERROR_STAT;
    TasksetState* ts = taskset_make(taskset);
    if (!ts) return CELL_SPURS_TASK_ERROR_NOMEM;
    u32 wid = 0;
    if (!spurs_allocate_workload(spurs_state, &wid)) {
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }
    memset(taskset, 0, size);
    ts->spurs = spurs;
    ts->args = args;
    ts->size = size;
    ts->wid = wid;
    ts->wid_registered = 1;
    ts->max_contention = max_contention;
    if (priority) memcpy(ts->priority, priority, sizeof(ts->priority));
    else memset(ts->priority, 1, sizeof(ts->priority));
    ts->shutdown = 0;
    memset(ts->tasks, 0, sizeof(ts->tasks));
    wr64((u8*)taskset + 0x60, guest_ea(spurs));
    wr64((u8*)taskset + 0x68, args);
    ((u8*)taskset)[0x70] = enable_clear_ls;
    ((u8*)taskset)[0x72] = 0x80; /* no task waits on the workload flag */
    wr32((u8*)taskset + 0x74, wid);
    if (size >= 0x1894) wr32((u8*)taskset + 0x1890, size);
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] taskset-create object=0x%08X "
                "wid=%u size=0x%X max=%u\n",
                guest_ea(taskset), wid, size, max_contention);
    return CELL_OK;
}
s32 cellSpursCreateTaskset(CellSpurs* s, CellSpursTaskset* ts, u64 args,
                           const u8* priority, u32 max)
{
    return create_taskset_common(s, ts, sizeof(*ts), args, priority, max, 0);
}
s32 cellSpursCreateTasksetWithAttribute(CellSpurs* s, CellSpursTaskset* ts,
                                        const CellSpursTasksetAttribute* a)
{
    if (!a) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    return create_taskset_common(s, ts, rd32(a->bytes + 0x20),
                                  rd64(a->bytes + 0x08), a->bytes + 0x10,
                                  rd32(a->bytes + 0x18),
                                  a->bytes[0x24] ? 1 : 0);
}
s32 cellSpursCreateTaskset2(CellSpurs* s, CellSpursTaskset2* ts,
                            const CellSpursTasksetAttribute2* a)
{
    u64 args = a ? rd64(a->bytes + 0x08) : 0;
    return create_taskset_common(s, ts, sizeof(*ts), args,
                                  a ? a->bytes + 0x10 : NULL,
                                  a ? rd32(a->bytes + 0x18) : 8,
                                  a && a->bytes[0x24] ? 1 : 0);
}
s32 cellSpursShutdownTaskset(CellSpursTaskset* object)
{
    if (!object) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    TasksetState* ts = taskset_find(object);
    if (!ts) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&ts->sync.mutex);
    ts->shutdown = 1;
    taskset_refresh_runnable_locked(ts);
    cv_wake_all(&ts->sync.cond);
    mx_unlock(&ts->sync.mutex);
    return CELL_OK;
}
s32 cellSpursJoinTaskset(CellSpursTaskset* object)
{
    TasksetState* ts = taskset_find(object);
    if (!ts) return object ? CELL_SPURS_TASK_ERROR_STAT : CELL_SPURS_TASK_ERROR_NULL_POINTER;
    mx_lock(&ts->sync.mutex);
    for (;;) {
        int pending = 0;
        for (u32 i = 0; i < 128; ++i)
            if (ts->tasks[i].thread_valid && !ts->tasks[i].complete) { pending = 1; break; }
        if (!pending) break;
        cv_wait(&ts->sync.cond, &ts->sync.mutex);
    }
    mx_unlock(&ts->sync.mutex);
    for (u32 i = 0; i < 128; ++i) task_join_thread(ts, &ts->tasks[i]);
    return CELL_OK;
}
s32 cellSpursDestroyTaskset(CellSpursTaskset* ts)
{
    s32 rc = cellSpursShutdownTaskset(ts);
    if (rc) return rc;
    rc = cellSpursJoinTaskset(ts);
    if (rc) return rc;
    TasksetState* state = taskset_find(ts);
    if (state) {
        mx_lock(&state->sync.mutex);
        const int release = state->wid_registered;
        state->wid_registered = 0;
        mx_unlock(&state->sync.mutex);
        if (release)
            spurs_release_workload(spurs_find(state->spurs), state->wid);
    }
    return CELL_OK;
}
s32 cellSpursDestroyTaskset2(CellSpursTaskset2* ts)
{
    return cellSpursDestroyTaskset((CellSpursTaskset*)ts);
}

enum {
    TA_REV=0x00, TA_SDK=0x04, TA_ELF=0x08, TA_CONTEXT=0x10,
    TA_CONTEXT_SIZE=0x18, TA_LSP=0x20, TA_ARG=0x30, TA_EXIT=0x40
};
s32 _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* a, u32 rev, u32 sdk,
                                      const void* elf, const void* save,
                                      const CellSpursTaskArgument* arg)
{
    if (!a || !elf || !arg) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes + TA_REV, rev);
    wr32(a->bytes + TA_SDK, sdk);
    wr64(a->bytes + TA_ELF, guest_ea(elf));
    if (save) {
        const u8* s = (const u8*)save;
        wr64(a->bytes + TA_CONTEXT, rd32(s + 0x00));
        wr32(a->bytes + TA_CONTEXT_SIZE, rd32(s + 0x04));
        u32 lsp_ea = rd32(s + 0x08);
        if (lsp_ea && vm_base) memcpy(a->bytes + TA_LSP, vm_base + lsp_ea, 16);
    }
    memcpy(a->bytes + TA_ARG, arg, 16);
    return CELL_OK;
}
s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* a)
{
    if (!a) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes + TA_REV, 1);
    return CELL_OK;
}
s32 cellSpursTaskAttributeSetExitCodeContainer(CellSpursTaskAttribute* a,
                                               CellSpursTaskExitCode* exit)
{
    if (!a || !exit) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    wr32(a->bytes + TA_EXIT, guest_ea(exit));
    return CELL_OK;
}
static s32 create_task_common(void* object, CellSpursTaskId* out, const u8* elf,
                              u32 context_ea, u32 context_size,
                              const u8 lsp[16], const u8 arg[16])
{
    TasksetState* ts = taskset_find(object);
    if (!ts || !out || !elf) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (ts->shutdown) return CELL_SPURS_TASK_ERROR_SHUTDOWN;
    size_t elf_size = spu_elf_image_size(elf, 16u * 1024u * 1024u);
    if (!elf_size) return CELL_SPURS_TASK_ERROR_NOEXEC;
    spu_workload_image resolved;
    if (!spu_workload_resolve(elf, (u32)elf_size, &resolved))
        return CELL_SPURS_TASK_ERROR_NOEXEC;

    mx_lock(&ts->sync.mutex);
    TaskState* task = NULL;
    for (u32 i = 0; i < 128; ++i) {
        if (!ts->tasks[i].thread_valid && !ts->tasks[i].complete) {
            task = &ts->tasks[i];
            memset(task, 0, sizeof(*task));
            task->id = i;
            break;
        }
    }
    if (!task) {
        mx_unlock(&ts->sync.mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    task->owner = ts;
    SpursState* spurs = spurs_find(ts->spurs);
    if (spurs) {
        mx_lock(&spurs->sync.mutex);
        if (spurs->nspus) {
            const u32 ordinal = spurs->next_spu_num++ % spurs->nspus;
            task->spu_num = spurs->nspus - 1u - ordinal;
        } else {
            task->spu_num = 0;
        }
        mx_unlock(&spurs->sync.mutex);
    }
    task->elf = elf;
    task->elf_size = (u32)elf_size;
    task->context_ea = context_ea;
    task->context_size = context_size;
    memcpy(task->ls_pattern, lsp, 16);
    memcpy(task->argument, arg, 16);
    task->image = resolved;
    task_publish(task);
    wr32(out, task->id);
    if (!task_start_thread(task)) {
        task_bit(ts->sync.key, 0x10, task->id, 0);
        task_bit(ts->sync.key, 0x30, task->id, 0);
        memset(task, 0, sizeof(*task));
        mx_unlock(&ts->sync.mutex);
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }
    taskset_refresh_runnable_locked(ts);
    mx_unlock(&ts->sync.mutex);
    return CELL_OK;
}
s32 cellSpursCreateTask(CellSpursTaskset* ts, CellSpursTaskId* out,
                        const void* elf, const void* context, u32 context_size,
                        const CellSpursTaskLsPattern* lsp,
                        const CellSpursTaskArgument* arg)
{
    static const u8 zero[16];
    return create_task_common(ts, out, (const u8*)elf, guest_ea(context),
                              context_size, lsp ? lsp->bytes : zero,
                              arg ? arg->bytes : zero);
}
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset* ts, CellSpursTaskId* out,
                                     const CellSpursTaskAttribute* a)
{
    if (!a || !vm_base) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    u32 elf = (u32)rd64(a->bytes + TA_ELF);
    return create_task_common(ts, out, vm_base + elf,
                              (u32)rd64(a->bytes + TA_CONTEXT),
                              rd32(a->bytes + TA_CONTEXT_SIZE),
                              a->bytes + TA_LSP, a->bytes + TA_ARG);
}
s32 cellSpursCreateTask2WithBinInfo(CellSpursTaskset2* ts, CellSpursTaskId* out,
                                    const CellSpursTaskBinInfo* bin,
                                    const CellSpursTaskArgument* arg,
                                    void* context, const char* name, void* reserved)
{
    (void)name;
    if (!bin || reserved || !vm_base) return CELL_SPURS_TASK_ERROR_INVAL;
    const u8* b = (const u8*)bin;
    u32 elf = (u32)rd64(b + 0x00);
    return create_task_common(ts, out, vm_base + elf, guest_ea(context),
                              rd32(b + 0x08), b + 0x10,
                              arg ? arg->bytes : (const u8[16]){0});
}
s32 _cellSpursSendSignal(CellSpursTaskset* object, CellSpursTaskId id)
{
    TasksetState* ts = taskset_find(object);
    if (!ts) return CELL_SPURS_TASK_ERROR_STAT;
    if (id >= 128 || !ts->tasks[id].thread_valid) return CELL_SPURS_TASK_ERROR_NOENT;
    mx_lock(&ts->sync.mutex);
    ts->tasks[id].signalled = 1;
    task_bit(object, 0x40, id, 1);
    cv_wake_all(&ts->sync.cond);
    mx_unlock(&ts->sync.mutex);
    return CELL_OK;
}
s32 cellSpursSendSignal(CellSpursTaskset* ts, CellSpursTaskId id)
{
    return _cellSpursSendSignal(ts, id);
}
static s32 join_task_common(void* object, u32 id, s32* exit, int try_only)
{
    TasksetState* ts = taskset_find(object);
    if (!ts) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&ts->sync.mutex);
    if (id >= 128 || (!ts->tasks[id].thread_valid && !ts->tasks[id].joined)) {
        mx_unlock(&ts->sync.mutex);
        return CELL_SPURS_TASK_ERROR_NOENT;
    }
    TaskState* task = &ts->tasks[id];
    if (try_only && !task->complete) {
        mx_unlock(&ts->sync.mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    while (!task->complete) cv_wait(&ts->sync.cond, &ts->sync.mutex);
    s32 code = task->exit_code;
    mx_unlock(&ts->sync.mutex);
    task_join_thread(ts, task);
    if (exit) wr32(exit, (u32)code);
    /* Joining consumes the completed task identity.  The public taskset has
     * 128 simultaneously addressable IDs, not a lifetime limit of 128 task
     * creations, so return this slot to the allocator after the host thread
     * has been reaped. */
    mx_lock(&ts->sync.mutex);
    if (!task->joined) {
        mx_unlock(&ts->sync.mutex);
        return CELL_SPURS_TASK_ERROR_NOENT;
    }
    memset(task, 0, sizeof(*task));
    mx_unlock(&ts->sync.mutex);
    return CELL_OK;
}
s32 cellSpursJoinTask2(CellSpursTaskset2* ts, u32 id, s32* exit)
{
    return join_task_common(ts, id, exit, 0);
}
s32 cellSpursTryJoinTask2(CellSpursTaskset2* ts, u32 id, s32* exit)
{
    return join_task_common(ts, id, exit, 1);
}
s32 cellSpursJoinTask(CellSpursTaskset* ts, u32 id, s32* exit)
{
    return join_task_common(ts, id, exit, 0);
}
s32 cellSpursTaskExitCodeTryGet(CellSpursTaskExitCode* object, s32* out)
{
    if (!object || !out) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!object->bytes[0]) return CELL_SPURS_TASK_ERROR_BUSY;
    wr32(out, rd32(object->bytes + 4));
    return CELL_OK;
}
s32 cellSpursTaskGetContextSaveAreaSize(u32* out, const CellSpursTaskLsPattern* lsp)
{
    if (!out || !lsp) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    wr32(out, 1024 + ls_pattern_blocks(lsp->bytes) * 2048);
    return CELL_OK;
}

/* ------------------------------------------------------------------------- */
/* Event flags                                                               */

static SyncKey g_event_flags[MAX_EVENT_FLAGS];
static void queue_notify_guest_write(u32 ea, u32 size);
static void jobchain_notify_guest_write(u32 ea, u32 size);
static void jobguard_notify_guest_write(u32 ea, u32 size);

void cellSpursNotifyGuestWrite(u32 ea, u32 size)
{
    if (g_registry_ready != 2 || !vm_base || !size) return;
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        TasksetState* ts = &g_tasksets[i];
        if (!ts->sync.live || !ts->sync.key) continue;
        const u64 object_ea = guest_ea(ts->sync.key);
        if (first >= object_ea + 128 || last <= object_ea) continue;
        int wake = 0;
        mx_lock(&ts->sync.mutex);
        const u8* object = (const u8*)ts->sync.key;
        for (u32 id = 0; id < CELL_SPURS_MAX_TASK; ++id) {
            const u8 mask = (u8)(0x80u >> (id & 7));
            if (!(object[0x40 + id / 8] & mask)) continue;
            TaskState* task = &ts->tasks[id];
            if (!task->thread_valid || task->complete) continue;
            task->signalled = 1;
            wake = 1;
        }
        if (wake) cv_wake_all(&ts->sync.cond);
        mx_unlock(&ts->sync.mutex);
    }
    for (u32 i = 0; i < MAX_EVENT_FLAGS; ++i) {
        SyncKey* sync = &g_event_flags[i];
        if (!sync->live || !sync->key) continue;
        const u64 object_ea = guest_ea(sync->key);
        if (first >= object_ea + 128 || last <= object_ea) continue;
        mx_lock(&sync->mutex);
        cv_wake_all(&sync->cond);
        mx_unlock(&sync->mutex);
    }
    queue_notify_guest_write(ea, size);
    jobguard_notify_guest_write(ea, size);
    jobchain_notify_guest_write(ea, size);
}

static SyncKey* sync_get(SyncKey* table, u32 count, void* key, int make)
{
    registry_init();
    mx_lock(&g_registry_mutex);
    SyncKey* result = NULL;
    for (u32 i = 0; i < count; ++i)
        if (table[i].live && table[i].key == key) { result = &table[i]; break; }
    if (!result && make) {
        for (u32 i = 0; i < count; ++i)
            if (!table[i].live) { sync_init(&table[i], key); result = &table[i]; break; }
    }
    mx_unlock(&g_registry_mutex);
    return result;
}
static int event_ready(u16 events, u16 mask, u32 mode)
{
    return mode == CELL_SPURS_EVENT_FLAG_AND ? (events & mask) == mask
                                             : (events & mask) != 0;
}

static TasksetState* event_waiter_taskset(const CellSpursEventFlag* ef,
                                          u32 slot)
{
    if (!vm_base || slot >= 16) return NULL;
    const u32 owner_ea = (u32)rd64(ef->bytes + 0x70);
    if (!owner_ea) return NULL;
    void* owner = vm_base + owner_ea;
    if (!ef->bytes[0x0d]) return taskset_find(owner);

    const u32 wid = ef->bytes[0x60 + slot];
    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        TasksetState* candidate = &g_tasksets[i];
        if (candidate->sync.live && candidate->spurs == owner &&
            candidate->wid_registered && candidate->wid == wid)
            return candidate;
    }
    return NULL;
}
s32 _cellSpursEventFlagInitialize(CellSpurs* spurs, CellSpursTaskset* ts,
                                  CellSpursEventFlag* ef, u32 clear, u32 direction)
{
    if (!ef || (!spurs && !ts)) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (clear > 1 || direction > 3) return CELL_SPURS_TASK_ERROR_INVAL;
    memset(ef->bytes, 0, 128);
    ef->bytes[0x0d] = spurs ? 1 : 0;
    ef->bytes[0x0e] = (u8)direction;
    ef->bytes[0x0f] = (u8)clear;
    ef->bytes[0x0c] = 0xff;
    wr64(ef->bytes + 0x70, guest_ea(spurs ? (void*)spurs : (void*)ts));
    return sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 1) ?
           CELL_OK : CELL_SPURS_TASK_ERROR_NOMEM;
}
s32 cellSpursEventFlagInitialize(CellSpursTaskset* ts, CellSpursEventFlag* ef,
                                 u32 clear, u32 direction)
{
    return _cellSpursEventFlagInitialize(NULL, ts, ef, clear, direction);
}
s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* ef)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&g_registry_mutex);
    if (ef->bytes[0x0c] != 0xff) {
        mx_unlock(&g_registry_mutex);
        return CELL_SPURS_TASK_ERROR_STAT;
    }
    u32 port = 16;
    for (; port < 64; ++port) {
        int used = 0;
        for (u32 i = 0; i < MAX_EVENT_FLAGS; ++i) {
            const SyncKey* candidate = &g_event_flags[i];
            if (candidate->live && candidate->key && candidate->key != ef &&
                ((const CellSpursEventFlag*)candidate->key)->bytes[0x0c] == port) {
                used = 1;
                break;
            }
        }
        if (!used) break;
    }
    if (port == 64) {
        mx_unlock(&g_registry_mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    const u32 object_id = (u32)(sync - g_event_flags) + 1;
    ef->bytes[0x0c] = (u8)port;
    wr32(ef->bytes + 0x78, object_id);
    wr32(ef->bytes + 0x7c, object_id);
    mx_unlock(&g_registry_mutex);
    return CELL_OK;
}
s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* ef)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    if (ef->bytes[0x0c] == 0xff) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&sync->mutex);
    if (rd16(ef->bytes + 0x04) || ef->bytes[0x07]) {
        mx_unlock(&sync->mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    ef->bytes[0x0c] = 0xff;
    wr32(ef->bytes + 0x78, 0);
    wr32(ef->bytes + 0x7c, 0);
    mx_unlock(&sync->mutex);
    return CELL_OK;
}
s32 cellSpursEventFlagSet(CellSpursEventFlag* ef, u16 bits)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_PPU2SPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    TasksetState* wake_tasksets[16] = {0};
    u8 wake_task_ids[16] = {0};
    u32 wake_count = 0;
    mx_lock(&sync->mutex);
    u16 events = (u16)(rd16(ef->bytes) | bits);
    u16 used = (u16)(rd16(ef->bytes + 0x08) & ~rd16(ef->bytes + 0x02));
    u16 modes = rd16(ef->bytes + 0x0a);
    u16 pending = 0, consumed = 0;
    for (u32 slot = 0; slot < 16; ++slot) {
        u16 slot_bit = (u16)(0x8000u >> slot);
        if (!(used & slot_bit)) continue;
        u16 mask = rd16(ef->bytes + 0x10 + slot * 2);
        u16 got = (u16)(events & mask);
        if (event_ready(events, mask, (modes & slot_bit) ? 1 : 0)) {
            wr16(ef->bytes + 0x30 + slot * 2, got);
            pending |= slot_bit;
            consumed |= got;
        }
    }
    wr16(ef->bytes + 0x02, (u16)(rd16(ef->bytes + 0x02) | pending));
    if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) events &= (u16)~consumed;
    wr16(ef->bytes, events);
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(pending & (0x8000u >> slot))) continue;
        TasksetState* taskset = event_waiter_taskset(ef, slot);
        if (taskset) {
            wake_tasksets[wake_count] = taskset;
            wake_task_ids[wake_count] = ef->bytes[0x50 + slot];
            ++wake_count;
        }
    }
    cv_wake_all(&sync->cond);
    mx_unlock(&sync->mutex);
    /* Do not nest a taskset mutex under the event-flag mutex.  Task code can
     * publish event-flag state while its scheduler owns the taskset mutex. */
    for (u32 i = 0; i < wake_count; ++i)
        _cellSpursSendSignal(
            (CellSpursTaskset*)wake_tasksets[i]->sync.key, wake_task_ids[i]);
    return CELL_OK;
}
static s32 event_wait(CellSpursEventFlag* ef, u16* bits, u32 mode, int try_only)
{
    if (!ef || !bits) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(ef, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (mode > 1) return CELL_SPURS_TASK_ERROR_INVAL;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    if (ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        ef->bytes[0x0e] != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    if (!try_only && ef->bytes[0x0c] == 0xff)
        return CELL_SPURS_TASK_ERROR_STAT;
    const u16 mask = rd16(bits);
    mx_lock(&sync->mutex);
    if (rd16(ef->bytes + 0x04) || ef->bytes[0x07]) {
        mx_unlock(&sync->mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    if (event_ready(rd16(ef->bytes), mask, mode)) {
        const u16 got = (u16)(rd16(ef->bytes) & mask);
        wr16(bits, got);
        if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~got));
        mx_unlock(&sync->mutex);
        return CELL_OK;
    }
    if (try_only) {
        mx_unlock(&sync->mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }

    u32 slot = 0;
    if (ef->bytes[0x0e] == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
        u16 used = rd16(ef->bytes + 0x08);
        u32 low_slot = 0;
        while (low_slot < 16 && (used & (1u << low_slot))) ++low_slot;
        if (low_slot == 16) {
            mx_unlock(&sync->mutex);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
        slot = 15 - low_slot;
        used |= (u16)(1u << low_slot);
        wr16(ef->bytes + 0x08, used);
    }
    ef->bytes[0x06] = (u8)((slot << 4) | mode);
    ef->bytes[0x07] = 0;
    wr16(ef->bytes + 0x04, mask);
    atomic_thread_fence(memory_order_release);

    while (!ef->bytes[0x07] && !event_ready(rd16(ef->bytes), mask, mode))
        cv_wait(&sync->cond, &sync->mutex);

    u16 got;
    if (ef->bytes[0x07]) {
        slot = ef->bytes[0x06] >> 4;
        got = rd16(ef->bytes + 0x30 + slot * 2);
        ef->bytes[0x07] = 0;
    } else {
        got = (u16)(rd16(ef->bytes) & mask);
        if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~got));
    }
    wr16(bits, got);
    wr16(ef->bytes + 0x04, 0);
    if (ef->bytes[0x0e] == CELL_SPURS_EVENT_FLAG_ANY2ANY)
        wr16(ef->bytes + 0x08,
             (u16)(rd16(ef->bytes + 0x08) & ~(0x8000u >> slot)));
    mx_unlock(&sync->mutex);
    return CELL_OK;
}
s32 cellSpursEventFlagWait(CellSpursEventFlag* ef, u16* bits, u32 mode)
{ return event_wait(ef, bits, mode, 0); }
s32 cellSpursEventFlagTryWait(CellSpursEventFlag* ef, u16* bits, u32 mode)
{ return event_wait(ef, bits, mode, 1); }
s32 cellSpursEventFlagClear(CellSpursEventFlag* ef, u16 bits)
{
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&sync->mutex);
    wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~bits));
    mx_unlock(&sync->mutex);
    return CELL_OK;
}

/* ------------------------------------------------------------------------- */
/* Queue and LFQueue                                                         */

typedef struct {
    SyncKey sync;
    u8* buffer;
    void* owner;
    u32 element_size, depth, direction;
    u32 iwl;
    int kind;
} QueueState;
static QueueState g_queues[MAX_QUEUES];
static _Atomic unsigned g_lfqueue_trace_dumps;

enum {
    QUEUE_KIND_SPURS = 1,
    QUEUE_KIND_LF = 2
};

extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
extern int spu_coh_is_reserved(u32);
extern void spu_coh_notify_write(u32);

static QueueState* queue_find(void* key)
{
    for (u32 i = 0; i < MAX_QUEUES; ++i)
        if (g_queues[i].sync.live && g_queues[i].sync.key == key) return &g_queues[i];
    return NULL;
}
static QueueState* queue_make(void* key)
{
    registry_init();
    mx_lock(&g_registry_mutex);
    QueueState* q = queue_find(key);
    if (!q) for (u32 i = 0; i < MAX_QUEUES; ++i) if (!g_queues[i].sync.live) {
        q = &g_queues[i]; memset(q, 0, sizeof(*q)); sync_init(&q->sync, key); break;
    }
    mx_unlock(&g_registry_mutex);
    return q;
}

static void lfqueue_trace_dump(const QueueState* q, const char* phase)
{
    if (!lfqueue_trace_enabled() ||
        atomic_fetch_add_explicit(&g_lfqueue_trace_dumps, 1,
                                  memory_order_relaxed) >= 8)
        return;
    const u8* object = (const u8*)q->sync.key;
    fprintf(stderr,
            "[native-spurs-lfqueue] %s queue=0x%08X owner=0x%08X "
            "dir=%u size=%u depth=%u\n",
            phase, guest_ea(object), guest_ea(q->owner), q->direction,
            q->element_size, q->depth);
    for (u32 off = 0; off < 128; off += 16) {
        fprintf(stderr, "[native-spurs-lfqueue] +%02X", off);
        for (u32 i = 0; i < 16; ++i) fprintf(stderr, " %02X", object[off + i]);
        fputc('\n', stderr);
    }
    TasksetState* ts = taskset_find(q->owner);
    if (ts) {
        mx_lock(&ts->sync.mutex);
        fprintf(stderr, "[native-spurs-lfqueue] waiting-tasks");
        for (u32 id = 0; id < CELL_SPURS_MAX_TASK; ++id)
            if (ts->tasks[id].thread_valid && ts->tasks[id].waiting)
                fprintf(stderr, " %u", id);
        fputc('\n', stderr);
        mx_unlock(&ts->sync.mutex);
    }
    fflush(stderr);
}

static TasksetState* lfqueue_waiter_taskset(const QueueState* q, u16 token)
{
    if (!q->iwl) return taskset_find(q->owner);
    const u32 wid = token >> 8;
    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        TasksetState* ts = &g_tasksets[i];
        if (ts->sync.live && ts->spurs == q->owner && ts->wid == wid)
            return ts;
    }
    return NULL;
}

static void lfqueue_signal_one_waiter(const QueueState* q, const void* object,
                                      u32 wait_list_offset)
{
    const u8* bytes = (const u8*)object;
    if (!rd16(bytes + wait_list_offset)) return;
    for (u32 slot = 0; slot < 15; ++slot) {
        const u16 token = rd16(bytes + wait_list_offset + 2 + slot * 2);
        if (!token) continue;
        const u32 task_id = token & 0xffu;
        TasksetState* ts = lfqueue_waiter_taskset(q, token);
        if (!ts || task_id >= CELL_SPURS_MAX_TASK) continue;
        mx_lock(&ts->sync.mutex);
        TaskState* task = &ts->tasks[task_id];
        if (task->thread_valid && !task->complete && task->waiting) {
            task->signalled = 1;
            task_bit(ts->sync.key, 0x40, task_id, 1);
            cv_wake_all(&ts->sync.cond);
            mx_unlock(&ts->sync.mutex);
            return;
        }
        mx_unlock(&ts->sync.mutex);
    }
}

static void queue_notify_range_locked(const void* address, u32 size)
{
    if (!size) return;
    const u32 first = guest_ea(address) & ~127u;
    const u32 last = (guest_ea(address) + size - 1) & ~127u;
    for (u32 line = first; ; line += 128) {
        if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        if (line == last) break;
    }
}

static void queue_notify_guest_write(u32 ea, u32 size)
{
    if (g_registry_ready != 2 || !vm_base || !size) return;
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 i = 0; i < MAX_QUEUES; ++i) {
        QueueState* q = &g_queues[i];
        if (!q->sync.live || !q->sync.key) continue;
        const u64 object_ea = guest_ea(q->sync.key);
        if (first >= object_ea + 128 || last <= object_ea) continue;
        mx_lock(&q->sync.mutex);
        cv_wake_all(&q->sync.cond);
        mx_unlock(&q->sync.mutex);
    }
}

static s32 spurs_queue_init(void* owner, void* spurs, void* taskset,
                            void* object, const void* buffer, u32 size,
                            u32 depth, u32 direction)
{
    if (!owner || !object || !buffer) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(object, 128) || !aligned(buffer, 16))
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!size || (size & 15) || !depth || depth >= 0x4000 || direction > 2)
        return CELL_SPURS_TASK_ERROR_INVAL;
    QueueState* q = queue_make(object);
    if (!q) return CELL_SPURS_TASK_ERROR_NOMEM;
    mx_lock(&q->sync.mutex);
    q->buffer = (u8*)buffer;
    q->owner = owner;
    q->element_size = size;
    q->depth = depth;
    q->direction = direction;
    q->iwl = 0;
    q->kind = QUEUE_KIND_SPURS;
    memset(object, 0, 128);
    wr32((u8*)object + 0x00, 0); /* completed consumer pointer */
    wr32((u8*)object + 0x04, 0); /* completed producer pointer */
    wr32((u8*)object + 0x08, size);
    wr32((u8*)object + 0x0c, depth);
    wr64((u8*)object + 0x10, guest_ea(buffer));
    wr32((u8*)object + 0x18, 0xffffffffu);
    wr32((u8*)object + 0x1c, direction);
    wr64((u8*)object + 0x60, guest_ea(taskset));
    wr64((u8*)object + 0x68, guest_ea(spurs));
    mx_unlock(&q->sync.mutex);
    return CELL_OK;
}

s32 _cellSpursQueueInitialize(CellSpurs* s, CellSpursTaskset* ts, CellSpursQueue* q,
                              const void* b, u32 size, u32 depth, u32 dir)
{
    if (!s && !ts) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    void* spurs = s;
    if (!spurs && ts && vm_base) {
        const u32 ea = (u32)rd64((const u8*)ts + 0x60);
        if (ea) spurs = vm_base + ea;
    }
    return spurs_queue_init(s ? (void*)s : (void*)ts, spurs, ts,
                            q, b, size, depth, dir);
}

static s32 lf_queue_init(void* owner_raw, void* object, const void* buffer,
                         u32 size, u32 depth, u32 direction)
{
    if (!owner_raw || !object || !buffer) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(object, 128) || !aligned(buffer, 16))
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!size || size > 0x4000 || (size & 15) || !depth ||
        depth > 0x7fff || direction > 3)
        return CELL_SPURS_TASK_ERROR_INVAL;

    const uintptr_t raw = (uintptr_t)owner_raw;
    const u32 iwl = (u32)(raw & 1);
    void* owner = (void*)(raw & ~(uintptr_t)1);
    QueueState* q = queue_make(object);
    if (!q) return CELL_SPURS_TASK_ERROR_NOMEM;
    mx_lock(&q->sync.mutex);
    q->buffer = (u8*)buffer;
    q->owner = owner;
    q->element_size = size;
    q->depth = depth;
    q->direction = direction;
    q->iwl = iwl;
    q->kind = QUEUE_KIND_LF;

    memset(object, 0, 128);
    wr32((u8*)object + 0x10, size);
    wr32((u8*)object + 0x14, depth);
    wr64((u8*)object + 0x18, guest_ea(buffer) | (direction == 3 ? 1u : 0u));
    memset((u8*)object + 0x20, 0xff, 4);
    wr32((u8*)object + 0x24, direction);
    if (direction == 3) {
        wr32((u8*)object + 0x28, 0xffffffffu);
        wr16((u8*)object + 0x30, 0xffff);
        wr16((u8*)object + 0x50, 0xffff);
    }
    wr64((u8*)object + 0x70, guest_ea(owner) | iwl);
    lfqueue_trace_dump(q, "initialize");
    mx_unlock(&q->sync.mutex);
    return CELL_OK;
}

s32 _cellSpursLFQueueInitialize(void* owner, CellSpursLFQueue* q, const void* b,
                                u32 size, u32 depth, u32 dir)
{ return lf_queue_init(owner, q, b, size, depth, dir); }

static int spurs_queue_snapshot(const QueueState* q, const void* object,
                                u32* head, u32* tail, u32* count)
{
    const s32 raw_head = (s32)rd32((const u8*)object + 0x00);
    const s32 raw_tail = (s32)rd32((const u8*)object + 0x04);
    const u32 period = q->depth * 2;
    if (raw_head < 0 || raw_tail < 0) return 0; /* peer has a reservation */
    if ((u32)raw_head >= period || (u32)raw_tail >= period) return -1;
    *head = (u32)raw_head;
    *tail = (u32)raw_tail;
    *count = (*tail + period - *head) % period;
    return *count <= q->depth ? 1 : -1;
}

static s32 spurs_queue_push(QueueState* q, void* object, const void* data,
                            int blocking)
{
    if (q->direction != 2) return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&q->sync.mutex);
    for (;;) {
        u32 head, tail, count;
        spu_lockline_lock();
        const int state = spurs_queue_snapshot(q, object, &head, &tail, &count);
        if (state < 0) {
            spu_lockline_unlock();
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_STAT;
        }
        if (state > 0 && count < q->depth) {
            const u32 slot = tail % q->depth;
            memcpy(q->buffer + slot * q->element_size, data, q->element_size);
            atomic_thread_fence(memory_order_release);
            wr32((u8*)object + 0x04, (tail + 1) % (q->depth * 2));
            queue_notify_range_locked(q->buffer + slot * q->element_size,
                                      q->element_size);
            queue_notify_range_locked(object, 128);
            spu_lockline_unlock();
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        cv_wait(&q->sync.cond, &q->sync.mutex);
    }
}

static s32 spurs_queue_pop(QueueState* q, void* object, void* data,
                           int peek, int blocking)
{
    if (q->direction != 1) return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&q->sync.mutex);
    for (;;) {
        u32 head, tail, count;
        spu_lockline_lock();
        const int state = spurs_queue_snapshot(q, object, &head, &tail, &count);
        if (state < 0) {
            spu_lockline_unlock();
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_STAT;
        }
        if (state > 0 && count) {
            const u32 slot = head % q->depth;
            atomic_thread_fence(memory_order_acquire);
            memcpy(data, q->buffer + slot * q->element_size, q->element_size);
            if (!peek) {
                wr32((u8*)object + 0x00, (head + 1) % (q->depth * 2));
                queue_notify_range_locked(object, 128);
            }
            spu_lockline_unlock();
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        cv_wait(&q->sync.cond, &q->sync.mutex);
    }
}

s32 cellSpursQueuePushBody(CellSpursQueue* q, const void* d, u32 block)
{
    QueueState* state = queue_find(q);
    if (!q || !d) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!state || state->kind != QUEUE_KIND_SPURS) return CELL_SPURS_TASK_ERROR_STAT;
    return spurs_queue_push(state, q, d, block != 0);
}
s32 cellSpursQueuePopBody(CellSpursQueue* q, void* d, u32 peek, u32 block)
{
    QueueState* state = queue_find(q);
    if (!q || !d) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!state || state->kind != QUEUE_KIND_SPURS) return CELL_SPURS_TASK_ERROR_STAT;
    return spurs_queue_pop(state, q, d, peek != 0, block != 0);
}

static u16 lf_advance(u16 pointer, u32 depth)
{
    return (u16)(((u32)pointer + 1 >= depth * 2) ? 0 : pointer + 1);
}

static s32 lf_queue_push(QueueState* q, void* object, const void* data,
                         int blocking)
{
    if (q->direction == 3) {
        diag("lfqueue-any2any-unsupported", object, q->direction);
        return CELL_SPURS_TASK_ERROR_NOSYS;
    }
    if (q->direction != 2) return CELL_SPURS_TASK_ERROR_PERM;
    mx_lock(&q->sync.mutex);
#if defined(YZ_SPURS_LOCK_ORDER_TEST)
    /* Test-only rendezvous: hold the queue mutex immediately before this
     * path requests the SPU lockline, reproducing the production lock edge. */
    {
        extern void yz_spurs_lock_order_test_queue_mutex_held(void);
        yz_spurs_lock_order_test_queue_mutex_held();
    }
#endif
    lfqueue_trace_dump(q, "push-before");
    for (;;) {
        spu_lockline_lock();
        const u16 consumed = rd16((const u8*)object + 0x00);
        const u16 completed = rd16((const u8*)object + 0x08);
        const u16 reserved = rd16((const u8*)object + 0x0e);
        const u32 period = q->depth * 2;
        const int valid = consumed < period && completed < period &&
                          reserved < period;
        const u32 count = valid ? (reserved + period - consumed) % period : 0;
        if (!valid || count > q->depth) {
            spu_lockline_unlock();
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_STAT;
        }
        if (reserved == completed && count < q->depth) {
            const u32 slot = reserved % q->depth;
            const u16 next = lf_advance(reserved, q->depth);
            memcpy(q->buffer + slot * q->element_size, data, q->element_size);
            atomic_thread_fence(memory_order_release);
            wr16((u8*)object + 0x08, next);
            wr16((u8*)object + 0x0a, 0);
            wr16((u8*)object + 0x0e, next);
            queue_notify_range_locked(q->buffer + slot * q->element_size,
                                      q->element_size);
            queue_notify_range_locked(object, 128);
            spu_lockline_unlock();
            /* A blocking SPU pop publishes its workload/task token in the
             * consumer wait list before entering SPURS WAITING.  Completing
             * a PPU-to-SPU push wakes exactly one published consumer; the
             * resumed queue routine then claims the now-visible entry and
             * retires its own wait record. */
            lfqueue_signal_one_waiter(q, object, 0x30);
            lfqueue_trace_dump(q, "push-after");
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        cv_wait(&q->sync.cond, &q->sync.mutex);
    }
}

s32 _cellSpursLFQueuePushBody(CellSpursLFQueue* q, const void* d, u32 block)
{
    QueueState* state = queue_find(q);
    if (!q || !d) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!state || state->kind != QUEUE_KIND_LF) return CELL_SPURS_TASK_ERROR_STAT;
    return lf_queue_push(state, q, d, block != 0);
}
s32 cellSpursQueueAttachLv2EventQueue(CellSpursQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_SPURS ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursQueueDetachLv2EventQueue(CellSpursQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_SPURS ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursLFQueueAttachLv2EventQueue(CellSpursLFQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_LF ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursLFQueueDetachLv2EventQueue(CellSpursLFQueue* q)
{ QueueState* s = queue_find(q); return s && s->kind == QUEUE_KIND_LF ? CELL_OK : CELL_SPURS_TASK_ERROR_STAT; }
s32 cellSpursBarrierInitialize(CellSpursTaskset* ts, CellSpursBarrier* b, u32 total)
{
    if (!ts || !b) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(b, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!total || total > 128) return CELL_SPURS_TASK_ERROR_INVAL;
    memset(b->bytes, 0, 128);
    wr32(b->bytes + 0x04, total);
    wr32(b->bytes + 0x34, guest_ea(ts));
    return CELL_OK;
}

/* ------------------------------------------------------------------------- */
/* Job chains and guards                                                     */

#define MAX_JOB_COMMAND_SNAPSHOT 512
#define MAX_JOB_PARKED_SLOTS 512
#define MAX_JOB_LEDGER_ENTRIES 128
typedef struct {
    u32 ea;
    u64 command;
    u64 publication_sequence;
    u16 descriptor_size;
    u8 descriptor_status; /* 0=not a job, 1=stable snapshot, 2=fault */
    u8 descriptor[0x400];
} JobCommandSnapshot;

typedef struct PendingJobSnapshot {
    struct PendingJobSnapshot* next;
    u64 sequence;
    u32 start_ea;
    u32 count;
    JobCommandSnapshot command[MAX_JOB_COMMAND_SNAPSHOT];
} PendingJobSnapshot;

typedef struct {
    _Atomic u32 sequence;
    u32 type;
    u32 ea;
    u32 aux;
    u64 value;
} JobChainLedgerEntry;

typedef struct JobChainState {
    SyncKey sync;
    void* spurs;
    u32 entry_ea, current_ea;
    u32 wid;
    u16 descriptor_size;
    u16 max_grab;
    int running, complete, error;
    _Atomic int shutdown;
    u32 call_stack[16];
    u32 call_depth;
    JobCommandSnapshot command_snapshot[MAX_JOB_COMMAND_SNAPSHOT];
    u32 command_snapshot_count;
    u32 command_snapshot_index;
    PendingJobSnapshot* pending_snapshot_head;
    PendingJobSnapshot* pending_snapshot_tail;
    u64 pending_snapshot_sequence;
    u32 watch_lo, watch_hi;
    u32 command_state_ea[MAX_JOB_PARKED_SLOTS];
    u32 command_state_count;
    u32 parked_slot_ea[MAX_JOB_PARKED_SLOTS];
    u64 parked_slot_empty[MAX_JOB_PARKED_SLOTS];
    u32 parked_slot_count;
    _Atomic u8 command_consumed[MAX_JOB_PARKED_SLOTS];
    u64 command_publication_sequence[MAX_JOB_PARKED_SLOTS];
    u64 deferred_resume_sequence[MAX_JOB_PARKED_SLOTS];
    u64 deferred_publication_sequence[MAX_JOB_PARKED_SLOTS];
    u64 deferred_sequence_next;
    JobChainLedgerEntry ledger[MAX_JOB_LEDGER_ENTRIES];
    _Atomic u32 ledger_next;
    int waiting_command;
    u32 alternate_slot;
    u32 dma_tag[2];
    u32 next_dma_tag;
    nthread thread;
    int thread_valid, joined;
} JobChainState;
typedef struct {
    SyncKey sync;
    JobChainState* chain;
    u32 count, original;
    u8 request_count, auto_reset;
} JobGuardState;
static JobChainState g_jobchains[MAX_JOBCHAINS];
static JobGuardState g_jobguards[MAX_JOB_GUARDS];
volatile u32 g_native_spurs_ppu_watch_lo = UINT32_MAX;
volatile u32 g_native_spurs_ppu_watch_hi = 0;

static void jobchain_watch_ea_locked(JobChainState* jc, u32 ea)
{
    if (!ea) return;
    if (ea < jc->watch_lo) jc->watch_lo = ea;
    if (ea <= 0xfffffff7u && ea + 8u > jc->watch_hi)
        jc->watch_hi = ea + 8u;
    if (jc->watch_lo < g_native_spurs_ppu_watch_lo)
        g_native_spurs_ppu_watch_lo = jc->watch_lo;
    if (jc->watch_hi > g_native_spurs_ppu_watch_hi)
        g_native_spurs_ppu_watch_hi = jc->watch_hi;
}

static void jobchain_ledger_record(JobChainState* jc, u32 type, u32 ea,
                                   u32 aux, u64 value)
{
    if (!jobchain_ledger_enabled()) return;
    const u32 sequence = atomic_fetch_add_explicit(
        &jc->ledger_next, 1, memory_order_relaxed) + 1;
    JobChainLedgerEntry* entry =
        &jc->ledger[(sequence - 1) % MAX_JOB_LEDGER_ENTRIES];
    atomic_store_explicit(&entry->sequence, 0, memory_order_relaxed);
    entry->type = type;
    entry->ea = ea;
    entry->aux = aux;
    entry->value = value;
    atomic_store_explicit(&entry->sequence, sequence, memory_order_release);
}

static int job_command_is_empty(u32 pc, u64 command)
{
    if (command == 0x0000000800000012ull)
        return 1;
    return (command & 7u) == 3u && (u32)(command & ~7ull) == pc;
}

static int job_command_is_publication_barrier(u64 command)
{
    /* SYNC/LWSYNC and their label forms publish the command body that
     * precedes the head store. */
    return command == 0x02ull || command == 0x0aull ||
           command == 0x12ull || command == 0x1aull;
}

static int job_command_starts_work(u64 command)
{
    const u32 op = (u32)(command & 7u);
    return (command && op == 0u) || op == 6u;
}

static int job_descriptor_copy_stable(u32 ea, u16 size, u8* destination)
{
    if (!size || size > 0x400u || !job_guest_range_valid(ea, size))
        return 0;
    u8 verify[0x400];
    for (u32 attempt = 0; attempt < 8; ++attempt) {
        memcpy(destination, vm_base + ea, size);
        atomic_thread_fence(memory_order_seq_cst);
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
        extern void yz_spurs_descriptor_snapshot_test_hook(
            u32, u32, u32);
        yz_spurs_descriptor_snapshot_test_hook(ea, size, attempt);
#endif
        memcpy(verify, vm_base + ea, size);
        atomic_thread_fence(memory_order_acquire);
        if (!memcmp(destination, verify, size))
            return 1;
        nthread_yield();
    }
    return 0;
}

static int jobchain_command_slot(JobChainState* jc, u32 ea, int create)
{
    if (!ea || (ea & 7u)) return -1;
    for (u32 i = 0; i < jc->command_state_count; ++i)
        if (jc->command_state_ea[i] == ea) return (int)i;
    if (!create || jc->command_state_count == MAX_JOB_PARKED_SLOTS)
        return -1;
    const u32 slot = jc->command_state_count++;
    jc->command_state_ea[slot] = ea;
    return (int)slot;
}

/*
 * A command-ring producer fills the body first and publishes its head last.
 * Claim the resulting immutable command view before returning from the guest
 * write notification.  The producer may reuse consumed slots while jobs run;
 * parsing directly from mutable guest memory after each synchronous host job
 * would therefore observe a mixture of two publications.
 */
static u32 jobchain_capture_commands(JobChainState* jc, u32 start_ea,
                                     JobCommandSnapshot* snapshot)
{
    u32 pc = start_ea;
    u32 stack[16];
    u32 depth = jc->call_depth;
    if (depth > 16) depth = 16;
    memcpy(stack, jc->call_stack, depth * sizeof(stack[0]));

    u32 count = 0;
    while (count < MAX_JOB_COMMAND_SNAPSHOT) {
        if (!job_guest_range_valid(pc, sizeof(u64)))
            break;
        int visited = 0;
        for (u32 i = 0; i < count; ++i) {
            if (snapshot[i].ea == pc) {
                visited = 1;
                break;
            }
        }
        if (visited)
            break;
        const u64 command = rd64(vm_base + pc);
        const u32 op = (u32)(command & 7u);
        const u32 ext = (u32)(command & 127u);
        if (job_command_is_empty(pc, command))
            break;

        snapshot[count].ea = pc;
        snapshot[count].command = command;
        snapshot[count].descriptor_size = 0;
        snapshot[count].descriptor_status = 0;
        {
            const int slot = jobchain_command_slot(jc, pc, 0);
            snapshot[count].publication_sequence = slot >= 0 ?
                jc->command_publication_sequence[slot] : 0;
        }
        if (command && op == 0u && jc->descriptor_size <= 0x400u) {
            const u32 descriptor_ea = (u32)command;
            snapshot[count].descriptor_status = 2;
            if (job_descriptor_copy_stable(descriptor_ea,
                                           jc->descriptor_size,
                                           snapshot[count].descriptor)) {
                snapshot[count].descriptor_size = jc->descriptor_size;
                snapshot[count].descriptor_status = 1;
            }
        }
        ++count;

        if ((command && op == 0u) || command == 0 || op == 2u ||
            op == 5u || op == 6u) {
            pc += 8;
            continue;
        }
        if (op == 1u || op == 3u) {
            if (op == 1u) depth = 0;
            pc = (u32)(command & ~7ull);
            continue;
        }
        if (op == 4u) {
            if (depth == 16) break;
            stack[depth++] = pc + 8;
            pc = (u32)(command & ~7ull);
            continue;
        }
        if (op == 7u) {
            if (ext == 119u) {
                if (!depth) break;
                pc = stack[--depth];
                continue;
            }
            if (ext == 15u || ext == 23u) {
                pc += 8;
                continue;
            }
            break;
        }
        break;
    }
    return count;
}

static u32 jobchain_claim_current(JobChainState* jc, u32 start_ea)
{
    const u32 count = jobchain_capture_commands(
        jc, start_ea, jc->command_snapshot);
    jc->command_snapshot_count = count;
    jc->command_snapshot_index = 0;
    jobchain_ledger_record(jc, 'C', start_ea, count,
                           count ? jc->command_snapshot[0].command : 0);
    return count;
}

static void jobchain_remember_parked_slot(JobChainState* jc, u32 ea,
                                          u64 empty_command)
{
    jobchain_watch_ea_locked(jc, ea);
    const int command_slot = jobchain_command_slot(jc, ea, 1);
    if (command_slot >= 0 && jc->running)
        atomic_store_explicit(&jc->command_consumed[command_slot], 1,
                              memory_order_release);
    for (u32 i = 0; i < jc->parked_slot_count; ++i) {
        if (jc->parked_slot_ea[i] == ea) {
            jc->parked_slot_empty[i] = empty_command;
            return;
        }
    }
    if (jc->parked_slot_count < MAX_JOB_PARKED_SLOTS) {
        const u32 i = jc->parked_slot_count++;
        jc->parked_slot_ea[i] = ea;
        jc->parked_slot_empty[i] = empty_command;
    }
}

static void jobchain_defer_resume(JobChainState* jc, u32 ea, u64 command,
                                  u64 publication_sequence)
{
    const int slot = jobchain_command_slot(jc, ea, 1);
    if (slot < 0 || jc->deferred_resume_sequence[slot]) return;
    const u64 sequence = ++jc->deferred_sequence_next;
    jc->deferred_resume_sequence[slot] = sequence;
    jc->deferred_publication_sequence[slot] = publication_sequence;
    jobchain_ledger_record(jc, 'R', ea, (u32)sequence, command);
}

static void jobchain_cancel_deferred_resume(JobChainState* jc, u32 ea,
                                            u64 publication_sequence)
{
    const int slot = jobchain_command_slot(jc, ea, 0);
    if (slot < 0 || !jc->deferred_resume_sequence[slot] ||
        jc->deferred_publication_sequence[slot] > publication_sequence)
        return;
    jobchain_ledger_record(jc, 'X', ea,
                           (u32)jc->deferred_resume_sequence[slot], 0);
    jc->deferred_resume_sequence[slot] = 0;
    jc->deferred_publication_sequence[slot] = 0;
}

static u32 jobchain_take_deferred_resume(JobChainState* jc)
{
    u64 oldest = UINT64_MAX;
    int oldest_slot = -1;
    for (u32 i = 0; i < MAX_JOB_PARKED_SLOTS; ++i) {
        const u64 sequence = jc->deferred_resume_sequence[i];
        if (sequence && sequence < oldest) {
            oldest = sequence;
            oldest_slot = (int)i;
        }
    }
    if (oldest_slot < 0) return 0;
    jc->deferred_resume_sequence[oldest_slot] = 0;
    jc->deferred_publication_sequence[oldest_slot] = 0;
    return jc->command_state_ea[oldest_slot];
}

static void jobchain_clear_queued_snapshots(JobChainState* jc)
{
    PendingJobSnapshot* snapshot = jc->pending_snapshot_head;
    while (snapshot) {
        PendingJobSnapshot* next = snapshot->next;
        free(snapshot);
        snapshot = next;
    }
    jc->pending_snapshot_head = NULL;
    jc->pending_snapshot_tail = NULL;
}

static u32 jobchain_queue_snapshot(JobChainState* jc, u32 start_ea)
{
    PendingJobSnapshot* snapshot =
        (PendingJobSnapshot*)malloc(sizeof(*snapshot));
    if (!snapshot) {
        jobchain_ledger_record(jc, 'F', start_ea, 0, 0);
        return 0;
    }
    snapshot->next = NULL;
    snapshot->start_ea = start_ea;
    snapshot->count = jobchain_capture_commands(
        jc, start_ea, snapshot->command);
    if (!snapshot->count) {
        free(snapshot);
        jobchain_ledger_record(jc, 'Q', start_ea, 0, 0);
        return 0;
    }
    snapshot->sequence = ++jc->pending_snapshot_sequence;
    if (jc->pending_snapshot_tail)
        jc->pending_snapshot_tail->next = snapshot;
    else
        jc->pending_snapshot_head = snapshot;
    jc->pending_snapshot_tail = snapshot;
    jobchain_ledger_record(jc, 'Q', start_ea, snapshot->count,
                           snapshot->command[0].command);
    return snapshot->count;
}

static u32 jobchain_take_queued_snapshot(JobChainState* jc, u32 start_ea)
{
    PendingJobSnapshot* previous = NULL;
    PendingJobSnapshot* snapshot = jc->pending_snapshot_head;
    while (snapshot && snapshot->start_ea != start_ea) {
        previous = snapshot;
        snapshot = snapshot->next;
    }
    if (!snapshot) return 0;
    if (previous)
        previous->next = snapshot->next;
    else
        jc->pending_snapshot_head = snapshot->next;
    if (jc->pending_snapshot_tail == snapshot)
        jc->pending_snapshot_tail = previous;
    memcpy(jc->command_snapshot, snapshot->command,
           snapshot->count * sizeof(jc->command_snapshot[0]));
    jc->command_snapshot_count = snapshot->count;
    jc->command_snapshot_index = 0;
    jobchain_ledger_record(jc, 'T', start_ea, snapshot->count,
                           jc->command_snapshot[0].command);
    const u32 count = snapshot->count;
    free(snapshot);
    return count;
}

static void jobchain_notify_guest_write(u32 ea, u32 size)
{
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        JobChainState* jc = &g_jobchains[i];
        if (!jc->sync.live) continue;
        const u64 chain_first = jc->watch_lo;
        const u64 chain_last = jc->watch_hi;
        if (first >= chain_last || last <= chain_first) continue;
        mx_lock(&jc->sync.mutex);
        u64 exact_publication_sequence = 0;
        if (size == sizeof(u64) && !(ea & 7u) &&
            job_guest_range_valid(ea, sizeof(u64))) {
            const u64 exact_command = rd64(vm_base + ea);
            jobchain_ledger_record(jc, 'N', ea, size, exact_command);
            /* Producers clear reusable circular slots explicitly.  Remember
             * every such slot, including ones the worker consumed without
             * ever parking on, so a later wrapped publication cannot start
             * behind current_ea unnoticed. */
            if (job_command_is_empty(ea, exact_command))
                jobchain_remember_parked_slot(jc, ea, exact_command);
            else if (job_command_starts_work(exact_command) ||
                     job_command_is_publication_barrier(exact_command)) {
                const int slot = jobchain_command_slot(jc, ea, 1);
                if (slot >= 0)
                    exact_publication_sequence =
                        ++jc->command_publication_sequence[slot];
            }
        }
        int exact_barrier_handled = 0;
        for (u32 p = 0; p < jc->parked_slot_count; ++p) {
            const u32 slot_ea = jc->parked_slot_ea[p];
            /* A PPU producer may publish a 64-bit command with one or two
             * narrower stores.  The write callback runs after each store, so
             * any overlap is enough to re-read and validate the completed
             * big-endian command.  Requiring one notification to cover all
             * eight bytes loses the common final-low-word publication. */
            if (first >= (u64)slot_ea + sizeof(u64) || last <= slot_ea)
                continue;
            if (!job_guest_range_valid(slot_ea, sizeof(u64)))
                continue;
            const u64 command = rd64(vm_base + slot_ea);
            if (command == jc->parked_slot_empty[p] ||
                job_command_is_empty(slot_ea, command))
                continue;
            const int resumable = job_command_starts_work(command) ||
                                  job_command_is_publication_barrier(command);
            if (slot_ea == ea &&
                job_command_is_publication_barrier(command))
                exact_barrier_handled = 1;
            const int command_slot = jobchain_command_slot(jc, slot_ea, 1);
            const u64 publication_sequence =
                slot_ea == ea && exact_publication_sequence ?
                    exact_publication_sequence :
                    (command_slot >= 0 ?
                        ++jc->command_publication_sequence[command_slot] : 0);
            const int reused = resumable && command_slot >= 0 ?
                atomic_exchange_explicit(&jc->command_consumed[command_slot],
                                         0, memory_order_acq_rel) : 0;
            u32 claimed = 0, queued = 0;
            if (jc->waiting_command && slot_ea == jc->current_ea) {
                claimed = jobchain_claim_current(jc, slot_ea);
                if (claimed)
                    jc->waiting_command = 0;
            } else if (jc->waiting_command && reused) {
                /* A circular producer can wrap and refill a consumed slot
                 * immediately behind the command at which the native worker
                 * is parked.  That newly published executable command is the
                 * next resume point; otherwise waking only at current_ea
                 * skips the first job of the wrapped batch. */
                jc->current_ea = slot_ea;
                claimed = jobchain_claim_current(jc, slot_ea);
                if (claimed)
                    jc->waiting_command = 0;
            } else if (!jc->waiting_command && reused) {
                /* A refill can either extend the generation currently being
                 * streamed or begin the next circular generation.  Remember
                 * every candidate in publication order.  Normal traversal
                 * cancels candidates it reaches; only candidates still
                 * pending at the empty boundary become resume points. */
                jobchain_defer_resume(jc, slot_ea, command,
                                      publication_sequence);
                if (job_command_is_publication_barrier(command))
                    queued = jobchain_queue_snapshot(jc, slot_ea);
            } else if (job_command_is_publication_barrier(command)) {
                /* Future publications must be claimed before the producer is
                 * allowed to reuse their body.  Ordinary NEXT/JOB maintenance
                 * is not a publication head and must not consume the bounded
                 * pending-snapshot slots. */
                queued = jobchain_queue_snapshot(jc, slot_ea);
            }
            if (native_trace_enabled()) {
                fprintf(stderr,
                        "[native-spurs-trace] jobchain-publish ea=0x%08X "
                        "size=%u head=0x%08X command=%016llX "
                        "claimed=%u queued=%u\n",
                        ea, size, slot_ea,
                        (unsigned long long)command, claimed, queued);
            }
        }
        /* The first publication of a circular slot can race ahead of the
         * native worker ever parking there.  A completed aligned 8-byte write
         * of the documented synchronization commands is itself sufficient to
         * identify a publication head; claim it even when it is not yet in
         * the remembered-slot set. */
        if (!exact_barrier_handled && size == sizeof(u64) && !(ea & 7u) &&
            job_guest_range_valid(ea, sizeof(u64))) {
            const u64 command = rd64(vm_base + ea);
            if (job_command_is_publication_barrier(command)) {
                const u32 queued = jobchain_queue_snapshot(jc, ea);
                if (native_trace_enabled()) {
                    fprintf(stderr,
                            "[native-spurs-trace] jobchain-barrier "
                            "ea=0x%08X command=%016llX queued=%u\n",
                            ea, (unsigned long long)command, queued);
                }
            }
        }
        cv_wake_all(&jc->sync.cond);
        mx_unlock(&jc->sync.mutex);
    }
}

void cellSpursNotifyPpuGuestWrite(u32 ea, u32 size)
{
    cellSpursNotifyGuestWrite(ea, size);
}

static JobChainState* jobchain_find(const void* key)
{
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i)
        if (g_jobchains[i].sync.live && g_jobchains[i].sync.key == key) return &g_jobchains[i];
    return NULL;
}
static JobGuardState* jobguard_find(const void* key)
{
    for (u32 i = 0; i < MAX_JOB_GUARDS; ++i)
        if (g_jobguards[i].sync.live && g_jobguards[i].sync.key == key) return &g_jobguards[i];
    return NULL;
}
static JobChainState* jobchain_make(void* key)
{
    registry_init(); mx_lock(&g_registry_mutex);
    JobChainState* jc = jobchain_find(key);
    if (!jc) for (u32 i = 0; i < MAX_JOBCHAINS; ++i) if (!g_jobchains[i].sync.live) {
        jc = &g_jobchains[i]; memset(jc, 0, sizeof(*jc)); sync_init(&jc->sync, key); break;
    }
    mx_unlock(&g_registry_mutex); return jc;
}
static JobGuardState* jobguard_make(void* key)
{
    registry_init(); mx_lock(&g_registry_mutex);
    JobGuardState* g = jobguard_find(key);
    if (!g) for (u32 i = 0; i < MAX_JOB_GUARDS; ++i) if (!g_jobguards[i].sync.live) {
        g = &g_jobguards[i]; memset(g, 0, sizeof(*g)); sync_init(&g->sync, key); break;
    }
    mx_unlock(&g_registry_mutex); return g;
}

static void jobguard_notify_guest_write(u32 ea, u32 size)
{
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 i = 0; i < MAX_JOB_GUARDS; ++i) {
        JobGuardState* g = &g_jobguards[i];
        if (!g->sync.live || !g->sync.key) continue;
        const u64 guard_ea = guest_ea(g->sync.key);
        if (first >= guard_ea + 4u || last <= guard_ea) continue;
        mx_lock(&g->sync.mutex);
        g->count = rd32(g->sync.key);
        if (!g->count)
            cv_wake_all(&g->sync.cond);
        mx_unlock(&g->sync.mutex);
    }
}

s32 _cellSpursJobChainAttributeInitialize(u32 revision, u32 sdk,
                                           CellSpursJobChainAttribute* a,
                                          const u64* entry, u16 size_desc,
                                          u16 max_grab, const u8* priority,
                                          u32 max_cont, u32 auto_request,
                                          u32 tag1, u32 tag2, u32 fixed,
                                          u32 max_desc, u32 initial_request)
{
    if (!a || !entry || !priority) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(a, 8) || !aligned(entry, 8)) return CELL_SPURS_JOB_ERROR_ALIGN;
    if ((size_desc != 64 && (size_desc < 128 || (size_desc & 127))) ||
        !max_grab || max_grab > 16 || max_cont > CELL_SPURS_MAX_SPU ||
        tag1 > 30 || tag2 > 30 || max_desc < 256 || max_desc > 1024 ||
        (max_desc & 127) || size_desc > max_desc ||
        (!auto_request && (!initial_request || initial_request > 255)))
        return CELL_SPURS_JOB_ERROR_INVAL;
    for (u32 i = 0; i < CELL_SPURS_MAX_SPU; ++i)
        if (priority[i] > 15) return CELL_SPURS_JOB_ERROR_INVAL;
    memset(a->bytes, 0, sizeof(a->bytes));
    wr32(a->bytes + 0x00, revision);
    wr32(a->bytes + 0x04, sdk);
    wr32(a->bytes + 0x08, guest_ea(entry));
    wr16(a->bytes + 0x0c, size_desc);
    wr16(a->bytes + 0x0e, max_grab);
    memcpy(a->bytes + 0x10, priority, 8);
    wr32(a->bytes + 0x18, max_cont);
    a->bytes[0x1c] = (u8)(auto_request != 0);
    wr32(a->bytes + 0x20, tag1);
    wr32(a->bytes + 0x24, tag2);
    a->bytes[0x28] = (u8)(fixed != 0);
    wr32(a->bytes + 0x2c, max_desc);
    wr32(a->bytes + 0x30, initial_request);
    return CELL_OK;
}
s32 cellSpursJobChainAttributeSetName(CellSpursJobChainAttribute* a, const char* name)
{
    if (!a || !name) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    wr32(a->bytes + 0x38, guest_ea(name));
    return CELL_OK;
}
s32 cellSpursCreateJobChainWithAttribute(CellSpurs* s, CellSpursJobChain* object,
                                           const CellSpursJobChainAttribute* a)
{
    if (!s || !object || !a) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(s, 128) || !aligned(object, 128) || !aligned(a, 8))
        return CELL_SPURS_JOB_ERROR_ALIGN;
    {
        const u32 entry_ea = rd32(a->bytes + 0x08);
        const u16 size_desc = rd16(a->bytes + 0x0c);
        const u16 max_grab = rd16(a->bytes + 0x0e);
        const u32 max_cont = rd32(a->bytes + 0x18);
        const u32 tag1 = rd32(a->bytes + 0x20);
        const u32 tag2 = rd32(a->bytes + 0x24);
        const u32 max_desc = rd32(a->bytes + 0x2c);
        const u32 initial_request = rd32(a->bytes + 0x30);
        if (!entry_ea) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
        if (entry_ea & 7) return CELL_SPURS_JOB_ERROR_ALIGN;
        if (!job_guest_range_valid(entry_ea, sizeof(u64)))
            return CELL_SPURS_JOB_ERROR_FAULT;
        if ((size_desc != 64 && (size_desc < 128 || (size_desc & 127))) ||
            !max_grab || max_grab > 16 || max_cont > CELL_SPURS_MAX_SPU ||
            tag1 > 30 || tag2 > 30 || max_desc < 256 || max_desc > 1024 ||
            (max_desc & 127) || size_desc > max_desc ||
            (!a->bytes[0x1c] && (!initial_request || initial_request > 255)))
            return CELL_SPURS_JOB_ERROR_INVAL;
        for (u32 i = 0; i < CELL_SPURS_MAX_SPU; ++i)
            if (a->bytes[0x10 + i] > 15) return CELL_SPURS_JOB_ERROR_INVAL;
    }
    SpursState* spurs_state = spurs_find(s);
    if (!spurs_state) return CELL_SPURS_JOB_ERROR_STAT;
    JobChainState* jc = jobchain_make(object);
    if (!jc) return CELL_SPURS_JOB_ERROR_NOMEM;
    u32 wid = 0;
    if (!spurs_allocate_workload(spurs_state, &wid)) {
        return CELL_SPURS_JOB_ERROR_NOMEM;
    }
    memset(object->bytes, 0, sizeof(object->bytes));
    jc->spurs = s;
    jc->entry_ea = rd32(a->bytes + 0x08);
    jc->descriptor_size = rd16(a->bytes + 0x0c);
    jc->max_grab = rd16(a->bytes + 0x0e);
    jc->wid = wid;
    jobchain_clear_queued_snapshots(jc);
    jc->pending_snapshot_sequence = 0;
    jc->watch_lo = jc->entry_ea;
    jc->watch_hi = jc->entry_ea + 8u;
    jc->command_state_count = 0;
    memset(jc->command_state_ea, 0, sizeof(jc->command_state_ea));
    jc->command_snapshot_count = jc->command_snapshot_index = 0;
    jc->parked_slot_count = 0;
    for (u32 i = 0; i < MAX_JOB_PARKED_SLOTS; ++i)
        atomic_store_explicit(&jc->command_consumed[i], 0,
                              memory_order_relaxed);
    memset(jc->command_publication_sequence, 0,
           sizeof(jc->command_publication_sequence));
    memset(jc->deferred_resume_sequence, 0,
           sizeof(jc->deferred_resume_sequence));
    memset(jc->deferred_publication_sequence, 0,
           sizeof(jc->deferred_publication_sequence));
    jc->deferred_sequence_next = 0;
    jc->waiting_command = 0;
    jc->current_ea = jc->entry_ea;
    jc->complete = jc->running = jc->error = 0;
    atomic_store(&jc->shutdown, 0);
    jc->alternate_slot = 0x4c00;
    jc->dma_tag[0] = rd32(a->bytes + 0x20);
    jc->dma_tag[1] = rd32(a->bytes + 0x24);
    jc->next_dma_tag = 0;
    wr64(object->bytes + 0x00, jc->entry_ea);
    object->bytes[0x23] = 0;
    object->bytes[0x24] = a->bytes[0x1c] ? 1 : 0;
    object->bytes[0x28] = (u8)rd32(a->bytes + 0x30);
    object->bytes[0x2a] = (u8)rd32(a->bytes + 0x20);
    object->bytes[0x2b] = (u8)rd32(a->bytes + 0x24);
    {
        const u32 max_desc = rd32(a->bytes + 0x2c);
        object->bytes[0x2c] = (a->bytes[0x28] ? 0x80u : 0u) |
            (u8)((max_desc >= 0x100 ? (max_desc - 0x100) / 128 : 0) << 4);
    }
    object->bytes[0x2d] = (u8)rd32(a->bytes + 0x00);
    wr16(object->bytes + 0x70, jc->max_grab);
    wr16(object->bytes + 0x72, jc->descriptor_size);
    wr32(object->bytes + 0x74, wid);
    wr64(object->bytes + 0x78, guest_ea(s));
    wr32(object->bytes + 0x90, rd32(a->bytes + 0x04));
    object->bytes[0x94] = a->bytes[0x3c] ? 2 : 0;
    jobchain_watch_ea_locked(jc, jc->entry_ea);
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] jobchain-create object=0x%08X "
                "entry=0x%08X wid=%u desc=0x%X commands=%016llX/%016llX\n",
                guest_ea(object), jc->entry_ea, jc->wid, jc->descriptor_size,
                (unsigned long long)rd64(vm_base + jc->entry_ea),
                (unsigned long long)rd64(vm_base + jc->entry_ea + 8));
    return CELL_OK;
}

static u32 job_alloc(u32* cursor, u32 limit, u32 size, u32 alignment)
{
    if (!size) return 0;
    u32 at = (*cursor + alignment - 1) & ~(alignment - 1);
    if (at > limit || size > limit - at) return 0;
    *cursor = at + size;
    return at;
}

static void job_publish_copy_internal(u32 ea, const void* source, u32 size,
                                      int notify_spurs)
{
    if (!size) return;
    extern void spu_lockline_lock(void);
    extern void spu_lockline_unlock(void);
    extern int spu_coh_is_reserved(u32);
    extern void spu_coh_notify_write(u32);
    const u32 first = ea & ~127u;
    const u32 last = (ea + size - 1) & ~127u;
    spu_lockline_lock();
    memcpy(vm_base + ea, source, size);
    for (u32 line = first; ; line += 128) {
        if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        if (line == last) break;
    }
    spu_lockline_unlock();
    if (notify_spurs)
        cellSpursNotifyGuestWrite(ea, size);
}

static void job_publish_copy(u32 ea, const void* source, u32 size)
{
    job_publish_copy_internal(ea, source, size, 1);
}

static void job_publish_u32(u32 ea, u32 value)
{
    u8 encoded[4];
    wr32(encoded, value);
    job_publish_copy_internal(ea, encoded, sizeof(encoded), 0);
}

static int job_return_syscall(spu_context* ctx, void* opaque)
{
    (void)ctx;
    (void)opaque;
    return -1;
}

static int job_io_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_NATIVE_SPURS_JOB_IO_TRACE") ? 1 : 0;
    return enabled;
}

static u32 job_io_trace_hash(const u8* bytes, u32 size)
{
    u32 hash = 2166136261u;
    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static int run_job(JobChainState* jc, u32 descriptor_ea, u32 descriptor_size,
                   const u8* acquired_descriptor)
{
    if (!vm_base || descriptor_ea > 0xfffffff0u)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    if (descriptor_size < 0x30 || descriptor_size > 0x400 ||
        (descriptor_size & 0x0f) ||
        descriptor_ea > 0xffffffffu - descriptor_size)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    if (!job_guest_range_valid(descriptor_ea, descriptor_size))
        return CELL_SPURS_JOB_ERROR_FAULT;
    u8 descriptor_copy[0x400];
    const u8* const guest_descriptor = vm_base + descriptor_ea;
    if (acquired_descriptor)
        memcpy(descriptor_copy, acquired_descriptor, descriptor_size);
    else if (!job_descriptor_copy_stable(descriptor_ea,
                                         (u16)descriptor_size,
                                         descriptor_copy))
        return CELL_SPURS_JOB_ERROR_FAULT;
    const u8* const d = descriptor_copy;
    u32 bin_ea = (u32)(rd64(d + 0x00) & ~1ull);
    u32 bin_size = (u32)rd16(d + 0x08) * 16;
    jobchain_ledger_record(jc, 'J', descriptor_ea, bin_size, bin_ea);
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] job-start descriptor=0x%08X "
                "bin=0x%08X size=0x%X slot=0x%05X\n",
                descriptor_ea, bin_ea, bin_size, jc->alternate_slot);
    if (!bin_ea || !bin_size) return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    if (!job_guest_range_valid(bin_ea, bin_size))
        return CELL_SPURS_JOB_ERROR_FAULT;
    const u32 dma_list_size = rd16(d + 0x0a);
    const u32 cache_list_size = rd32(d + 0x24);
    if ((dma_list_size & 7) || (cache_list_size & 7) ||
        dma_list_size + cache_list_size > descriptor_size - 0x30 ||
        cache_list_size / 8 > 4)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    spu_workload_image image;
    if (!spu_workload_resolve(vm_base + bin_ea, bin_size, &image))
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    spu_context* ctx = (spu_context*)malloc(sizeof(*ctx));
    if (!ctx) return CELL_SPURS_JOB_ERROR_NOMEM;
    spu_context_init(ctx, 0);
    u32 slot = jc->alternate_slot;
    jc->alternate_slot = slot == 0x4c00 ? 0xe400 : 0x4c00;
    /* Current title lifts give overlapping placements distinct image ids.
     * Select from the exact descriptor identity and the native scheduler's
     * chosen LS slot before entering the lifted job. */
    const int slot_image =
        spu_job_descriptor_image(image.image_id, slot, bin_ea);
    if (slot_image >= 0) image.image_id = slot_image;
    if (bin_size > SPU_LS_SIZE - slot) { native_spu_context_free(ctx); return CELL_SPURS_JOB_ERROR_INVALID_BIN; }
    memcpy(ctx->ls + slot, vm_base + bin_ea, bin_size);

    const u32 context_ls = 0x4940;
    const u32 descriptor_ls = 0x3f000;
    const u32 size_io = rd32(d + 0x14) & 0x3ffffu;
    const u32 size_out = rd32(d + 0x18) & 0x3ffffu;
    const u32 size_stack = rd16(d + 0x1c) ?
                           (u32)rd16(d + 0x1c) * 16u : 8192u;
    const u32 size_scratch = (u32)rd16(d + 0x1e) * 16u;
    u32 cursor = (slot + bin_size + 1023u) & ~1023u;
    const u32 io_ls = job_alloc(&cursor, descriptor_ls, size_io, 1024);
    const u32 out_ls = job_alloc(&cursor, descriptor_ls, size_out, 1024);
    u32 cache_ls[4] = {0, 0, 0, 0};
    u32 cache_count = cache_list_size / 8;
    for (u32 i = 0; i < cache_count; ++i) {
        u64 item = rd64(d + 0x30 + dma_list_size + i * 8);
        u32 item_size = (u32)(item >> 32) & 0x7fffu;
        cache_ls[i] = job_alloc(&cursor, descriptor_ls, item_size, 1024);
        if (item_size && !cache_ls[i]) { native_spu_context_free(ctx); return CELL_SPURS_JOB_ERROR_NOMEM; }
        if (item_size && !job_guest_range_valid((u32)item, item_size)) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_FAULT;
        }
        if (item_size) memcpy(ctx->ls + cache_ls[i], vm_base + (u32)item, item_size);
    }
    const u32 scratch_stack_ls =
        job_alloc(&cursor, descriptor_ls, size_scratch + size_stack, 1024);
    if ((size_io && !io_ls) || (size_out && !out_ls) || !scratch_stack_ls) {
        native_spu_context_free(ctx);
        return CELL_SPURS_JOB_ERROR_NOMEM;
    }

    u32 io_offset = 0;
    const u32 io_count = dma_list_size / 8;
    static atomic_uint job_io_trace_count;
    const unsigned job_io_trace_index =
        (job_io_trace_enabled() && bin_ea == 0x01252680u)
            ? atomic_fetch_add(&job_io_trace_count, 1u)
            : ~0u;
    if (job_io_trace_index < 32u) {
        fprintf(stderr,
                "[native-job-io] #%u desc=%08X size=%u bin=%08X/%X "
                "io=%05X/%X out=%05X/%X list=%u inout=%u type=%u "
                "user=%08X/%08X\n",
                job_io_trace_index, descriptor_ea, descriptor_size,
                bin_ea, bin_size, io_ls, size_io, out_ls, size_out,
                io_count, rd32(d + 0x10), d[0x2c],
                descriptor_size >= 0x38 ? rd32(d + 0x30) : 0,
                descriptor_size >= 0x38 ? rd32(d + 0x34) : 0);
    }
    for (u32 i = 0; i < io_count; ++i) {
        u64 item = rd64(d + 0x30 + i * 8);
        u32 item_size = (u32)(item >> 32) & 0x7fffu;
        u32 item_ea = (u32)item;
        const u32 item_flags = (u32)(item >> 32);
        const u32 item_alignment = item_size < 16u ? item_size : 16u;
        const u32 item_slot_size = (item_size + 15u) & ~15u;
        const u32 item_ls_offset = io_offset + (item_ea & 15u);
        if (job_io_trace_index < 32u) {
            fprintf(stderr,
                    "[native-job-io] #%u get[%u] ea=%08X size=%X "
                    "guest0=%08X guest4=%08X ls=%05X\n",
                    job_io_trace_index, i, item_ea, item_size,
                    item_size >= 4 ? rd32(vm_base + item_ea) : 0,
                    item_size >= 8 ? rd32(vm_base + item_ea + 4) : 0,
                    io_ls + item_ls_offset);
        }
        if (item_size > 0x4000u || (item_flags & 0x80000000u) ||
            (item_size && !item_ea) ||
            (item_size < 16u && item_size != 0u && item_size != 1u &&
             item_size != 2u && item_size != 4u && item_size != 8u) ||
            (item_alignment && (item_ea & (item_alignment - 1u))) ||
            io_offset > size_io || item_slot_size > size_io - io_offset ||
            (item_size && item_ls_offset + item_size >
                              io_offset + item_slot_size)) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
        }
        if (item_size && !job_guest_range_valid(item_ea, item_size)) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_FAULT;
        }
        if (job_io_trace_enabled() && item_size && item_size < 16u) {
            static atomic_uint sub_qword_trace_count;
            const unsigned trace = atomic_fetch_add(
                &sub_qword_trace_count, 1u);
            if (trace < 64u) {
                fprintf(stderr,
                        "[native-job-subq] #%u desc=%08X bin=%08X "
                        "item=%u ea=%08X size=%u slot=%05X payload=%05X\n",
                        trace, descriptor_ea, bin_ea, i, item_ea, item_size,
                        io_ls + io_offset, io_ls + item_ls_offset);
            }
        }
        /* For sub-qword list elements, libspurs preserves the effective
         * address low nibble in the element's 16-byte LS slot.  Jobs use
         * that placement when resolving list pointers. */
        if (item_size) memcpy(ctx->ls + io_ls + item_ls_offset,
                              vm_base + item_ea, item_size);
        io_offset += item_slot_size;
    }

    memcpy(ctx->ls + descriptor_ls, d, descriptor_size);
    memset(ctx->ls + context_ls, 0, 48);
    wr32(ctx->ls + context_ls + 0x00, io_ls);
    for (u32 i = 0; i < 4; ++i)
        wr32(ctx->ls + context_ls + 0x04 + i * 4, cache_ls[i]);
    wr32(ctx->ls + context_ls + 0x14,
         descriptor_size == 64 ? 0 : (descriptor_size / 128) << 28);
    wr16(ctx->ls + context_ls + 0x18, (u16)io_count);
    wr16(ctx->ls + context_ls + 0x1a, (u16)cache_count);
    wr32(ctx->ls + context_ls + 0x1c, out_ls);
    wr32(ctx->ls + context_ls + 0x20, scratch_stack_ls);
    wr32(ctx->ls + context_ls + 0x24,
         jc->dma_tag[jc->next_dma_tag & 1u]);
    jc->next_dma_tag ^= 1u;
    wr64(ctx->ls + context_ls + 0x28, descriptor_ea);

    const u32 job_io_trace_input_hash = job_io_trace_index < 32u
        ? job_io_trace_hash(ctx->ls + io_ls, size_io) : 0;

    ctx->gpr[0]._u32[0] = 0x0a70;
    ctx->gpr[1]._u32[0] = scratch_stack_ls + size_scratch + size_stack;
    ctx->gpr[3]._u32[0] = context_ls;
    ctx->gpr[4]._u32[0] = descriptor_ls;
    ctx->native_spurs_syscall = job_return_syscall;
    ctx->pc = slot;
    image.entry_pc = slot;
    int ok = spu_workload_execute(&image, ctx);

    if (job_io_trace_index < 32u) {
        fprintf(stderr,
                "[native-job-io] #%u result ok=%d pc=%05X tag=%u "
                "input=%08X->%08X output=%08X\n",
                job_io_trace_index, ok, ctx->pc,
                rd32(ctx->ls + context_ls + 0x24),
                job_io_trace_input_hash,
                job_io_trace_hash(ctx->ls + io_ls, size_io),
                job_io_trace_hash(ctx->ls + out_ls, size_out));
        io_offset = 0;
        for (u32 i = 0; i < io_count; ++i) {
            const u64 item = rd64(d + 0x30 + i * 8);
            const u32 item_size = (u32)(item >> 32) & 0x7fffu;
            const u32 item_ea = (u32)item;
            const u32 item_ls_offset = io_offset + (item_ea & 15u);
            fprintf(stderr,
                    "[native-job-io] #%u done[%u] ok=%d pc=%05X "
                    "ls0=%08X ls4=%08X desc10=%08X guest10=%08X\n",
                    job_io_trace_index, i, ok, ctx->pc,
                    item_size >= 4 ? rd32(ctx->ls + io_ls + item_ls_offset) : 0,
                    item_size >= 8 ? rd32(ctx->ls + io_ls + item_ls_offset + 4) : 0,
                    rd32(ctx->ls + descriptor_ls + 0x10),
                    rd32(guest_descriptor + 0x10));
            io_offset += (item_size + 15u) & ~15u;
        }
    }

    if (ok && rd32(d + 0x10)) {
        io_offset = 0;
        for (u32 i = 0; i < io_count; ++i) {
            u64 item = rd64(d + 0x30 + i * 8);
            u32 item_size = (u32)(item >> 32) & 0x7fffu;
            const u32 item_ea = (u32)item;
            const u32 item_ls_offset = io_offset + (item_ea & 15u);
            if (item_size)
                job_publish_copy(item_ea,
                                 ctx->ls + io_ls + item_ls_offset,
                                 item_size);
            if (job_io_trace_index < 32u && item_size) {
                const u32 guest_hash =
                    job_io_trace_hash(vm_base + item_ea, item_size);
                const u32 ls_hash = job_io_trace_hash(
                    ctx->ls + io_ls + item_ls_offset, item_size);
                fprintf(stderr,
                        "[native-job-io] #%u publish[%u] match=%u "
                        "guest=%08X ls=%08X\n",
                        job_io_trace_index, i, guest_hash == ls_hash,
                        guest_hash, ls_hash);
            }
            io_offset += (item_size + 15u) & ~15u;
        }
    }
    native_spu_context_free(ctx);
    jobchain_ledger_record(jc, 'D', descriptor_ea,
                           ok ? CELL_OK : CELL_SPURS_JOB_ERROR_NOEXEC, bin_ea);
    return ok ? CELL_OK : CELL_SPURS_JOB_ERROR_NOEXEC;
}
static int wait_guard(JobChainState* jc, u32 guard_ea)
{
    if (!vm_base) return CELL_SPURS_JOB_ERROR_INVAL;
    if (!job_guest_range_valid(guard_ea, 128))
        return CELL_SPURS_JOB_ERROR_FAULT;
    JobGuardState* g = jobguard_find(vm_base + guard_ea);
    if (!g) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&g->sync.mutex);
    const int parked = g->count && !atomic_load(&jc->shutdown);
    if (parked)
        spurs_set_workload_runnable(jc->spurs, jc->wid, 0);
    while (g->count && !atomic_load(&jc->shutdown))
        cv_wait(&g->sync.cond, &g->sync.mutex);
    if (!g->count && g->auto_reset) {
        g->count = g->original;
        job_publish_u32(guest_ea(g->sync.key), g->count);
    }
    if (parked && !atomic_load(&jc->shutdown))
        spurs_set_workload_runnable(jc->spurs, jc->wid, 1);
    mx_unlock(&g->sync.mutex);
    return CELL_OK;
}
static int wait_job_slot(JobChainState* jc, u32 pc, u64 empty_command)
{
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] jobchain-park pc=0x%08X "
                "command=%016llX\n", pc,
                (unsigned long long)empty_command);
    mx_lock(&jc->sync.mutex);
    jobchain_ledger_record(jc, 'P', pc, 0, empty_command);
    jc->current_ea = pc;
    jobchain_watch_ea_locked(jc, pc);
    jc->command_snapshot_count = 0;
    jc->command_snapshot_index = 0;
    jc->waiting_command = 1;
    jobchain_remember_parked_slot(jc, pc, empty_command);
    for (;;) {
        const u32 resume_ea = jobchain_take_deferred_resume(jc);
        if (!resume_ea) break;
        jc->current_ea = resume_ea;
        const u32 claimed = jobchain_take_queued_snapshot(jc, resume_ea) ||
                            jobchain_claim_current(jc, resume_ea);
        jobchain_ledger_record(jc, 'A', resume_ea, claimed, 0);
        if (claimed) {
            jc->waiting_command = 0;
            break;
        }
        jc->current_ea = pc;
    }
    if (jc->waiting_command && jobchain_take_queued_snapshot(jc, pc))
        jc->waiting_command = 0;
    else if (jc->waiting_command && rd64(vm_base + pc) != empty_command &&
             jobchain_claim_current(jc, pc))
        jc->waiting_command = 0;
    const int parked = jc->waiting_command &&
                       rd64(vm_base + pc) == empty_command &&
                       !atomic_load(&jc->shutdown);
    if (parked)
        spurs_set_workload_runnable(jc->spurs, jc->wid, 0);
    while (jc->waiting_command && rd64(vm_base + pc) == empty_command &&
           !atomic_load(&jc->shutdown))
        cv_wait(&jc->sync.cond, &jc->sync.mutex);
    if (parked && !atomic_load(&jc->shutdown))
        spurs_set_workload_runnable(jc->spurs, jc->wid, 1);
    if (jc->waiting_command && !atomic_load(&jc->shutdown)) {
        if (jobchain_take_queued_snapshot(jc, pc) ||
            jobchain_claim_current(jc, pc))
            jc->waiting_command = 0;
    }
    if (atomic_load(&jc->shutdown))
        jc->waiting_command = 0;
    mx_unlock(&jc->sync.mutex);
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] jobchain-wake pc=0x%08X "
                "command=%016llX snapshot=%u\n", pc,
                (unsigned long long)(jc->command_snapshot_count
                    ? jc->command_snapshot[0].command
                    : rd64(vm_base + pc)),
                jc->command_snapshot_count);
    return CELL_OK;
}

static u32 jobchain_get_current(JobChainState* jc)
{
    mx_lock(&jc->sync.mutex);
    const u32 pc = jc->current_ea;
    mx_unlock(&jc->sync.mutex);
    return pc;
}

static void jobchain_set_current(JobChainState* jc, u32 pc)
{
    mx_lock(&jc->sync.mutex);
    jc->current_ea = pc;
    jobchain_watch_ea_locked(jc, pc);
    mx_unlock(&jc->sync.mutex);
}

static void jobchain_reset_calls(JobChainState* jc)
{
    mx_lock(&jc->sync.mutex);
    jc->call_depth = 0;
    mx_unlock(&jc->sync.mutex);
}

static int jobchain_push_call(JobChainState* jc, u32 return_ea)
{
    int ok = 0;
    mx_lock(&jc->sync.mutex);
    if (jc->call_depth < 16) {
        jc->call_stack[jc->call_depth++] = return_ea;
        ok = 1;
    }
    mx_unlock(&jc->sync.mutex);
    return ok;
}

static int jobchain_pop_call(JobChainState* jc, u32* return_ea)
{
    int ok = 0;
    mx_lock(&jc->sync.mutex);
    if (jc->call_depth) {
        *return_ea = jc->call_stack[--jc->call_depth];
        ok = 1;
    }
    mx_unlock(&jc->sync.mutex);
    return ok;
}

void cellSpursDumpNativeJobChains(const char* tag)
{
    const char* label = tag ? tag : "snapshot";
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        JobChainState* jc = &g_jobchains[i];
        if (!jc->sync.live) continue;
        mx_lock(&jc->sync.mutex);
        fprintf(stderr,
                "[native-spurs-ledger] %s chain=0x%08X entry=0x%08X "
                "current=0x%08X run=%d complete=%d error=0x%08X "
                "wait=%d active=%u/%u parked=%u\n",
                label, guest_ea(jc->sync.key), jc->entry_ea, jc->current_ea,
                jc->running, jc->complete, (u32)jc->error,
                jc->waiting_command, jc->command_snapshot_index,
                jc->command_snapshot_count, jc->parked_slot_count);
        if (jc->command_snapshot_count) {
            const JobCommandSnapshot* first = &jc->command_snapshot[0];
            const JobCommandSnapshot* last =
                &jc->command_snapshot[jc->command_snapshot_count - 1];
            fprintf(stderr,
                    "[native-spurs-ledger] active first=%08X/%016llX "
                    "last=%08X/%016llX\n",
                    first->ea, (unsigned long long)first->command,
                    last->ea, (unsigned long long)last->command);
        }
        for (const PendingJobSnapshot* pending = jc->pending_snapshot_head;
             pending; pending = pending->next) {
            const JobCommandSnapshot* first = &pending->command[0];
            const JobCommandSnapshot* last =
                &pending->command[pending->count - 1];
            fprintf(stderr,
                    "[native-spurs-ledger] pending[%llu] start=%08X count=%u "
                    "first=%08X/%016llX last=%08X/%016llX\n",
                    (unsigned long long)pending->sequence, pending->start_ea,
                    pending->count,
                    first->ea, (unsigned long long)first->command,
                    last->ea, (unsigned long long)last->command);
        }
        const u32 end = atomic_load_explicit(&jc->ledger_next,
                                              memory_order_acquire);
        const u32 begin = end > MAX_JOB_LEDGER_ENTRIES ?
                          end - MAX_JOB_LEDGER_ENTRIES : 0;
        for (u32 sequence = begin + 1; sequence <= end; ++sequence) {
            const JobChainLedgerEntry* entry =
                &jc->ledger[(sequence - 1) % MAX_JOB_LEDGER_ENTRIES];
            if (atomic_load_explicit(&entry->sequence,
                                     memory_order_acquire) != sequence)
                continue;
            fprintf(stderr,
                    "[native-spurs-ledger] #%u %c ea=%08X aux=%08X "
                    "value=%016llX\n",
                    sequence, (int)entry->type, entry->ea, entry->aux,
                    (unsigned long long)entry->value);
        }
        fflush(stderr);
        mx_unlock(&jc->sync.mutex);
    }
}
static int execute_chain(JobChainState* jc)
{
    u32 pc = jobchain_get_current(jc);
    if (!vm_base || !pc) return CELL_SPURS_JOB_ERROR_INVAL;
    u32 dispatch_count = 0;
    jobchain_reset_calls(jc);
    while (!atomic_load(&jc->shutdown)) {
        if (!job_guest_range_valid(pc, sizeof(u64)))
            return CELL_SPURS_JOB_ERROR_FAULT;
        if ((++dispatch_count & 0xffffu) == 0)
            nthread_yield();
        u64 cmd;
        u64 publication_sequence = 0;
        const u8* acquired_descriptor = NULL;
        int acquired_descriptor_fault = 0;
        mx_lock(&jc->sync.mutex);
        if (jc->command_snapshot_index < jc->command_snapshot_count &&
            jc->command_snapshot[jc->command_snapshot_index].ea == pc) {
            cmd = jc->command_snapshot[jc->command_snapshot_index].command;
            publication_sequence =
                jc->command_snapshot[jc->command_snapshot_index]
                    .publication_sequence;
            if (jc->command_snapshot[jc->command_snapshot_index]
                    .descriptor_size == jc->descriptor_size)
                acquired_descriptor =
                    jc->command_snapshot[jc->command_snapshot_index]
                        .descriptor;
            else if (jc->command_snapshot[jc->command_snapshot_index]
                         .descriptor_status == 2)
                acquired_descriptor_fault = 1;
            ++jc->command_snapshot_index;
        } else {
            jc->command_snapshot_count = 0;
            jc->command_snapshot_index = 0;
            /* A publication barrier can be captured before the worker reaches
             * it.  Consume that immutable view on the ordinary execution
             * path too, not only after parking at the same address. */
            if (jobchain_take_queued_snapshot(jc, pc)) {
                cmd = jc->command_snapshot[0].command;
                publication_sequence =
                    jc->command_snapshot[0].publication_sequence;
                if (jc->command_snapshot[0].descriptor_size ==
                    jc->descriptor_size)
                    acquired_descriptor =
                        jc->command_snapshot[0].descriptor;
                else if (jc->command_snapshot[0].descriptor_status == 2)
                    acquired_descriptor_fault = 1;
                jc->command_snapshot_index = 1;
            } else {
                cmd = rd64(vm_base + pc);
                const int command_slot = jobchain_command_slot(jc, pc, 0);
                if (command_slot >= 0)
                    publication_sequence =
                        jc->command_publication_sequence[command_slot];
            }
        }
        jobchain_cancel_deferred_resume(jc, pc, publication_sequence);
        {
            const int command_slot = jobchain_command_slot(jc, pc, 0);
            if (command_slot >= 0)
                atomic_store_explicit(&jc->command_consumed[command_slot], 1,
                                      memory_order_release);
        }
        jobchain_watch_ea_locked(jc, pc);
        if (pc <= 0xfffffff7u)
            jobchain_watch_ea_locked(jc, pc + 8u);
        mx_unlock(&jc->sync.mutex);
        u32 op = (u32)(cmd & 7), ext = (u32)(cmd & 127);
        if (cmd == 0x0000000800000012ull) {
            int rc = wait_job_slot(jc, pc, cmd);
            if (rc) return rc;
            pc = jobchain_get_current(jc);
            continue;
        }
        if (cmd && op == 0) {
            if (acquired_descriptor_fault)
                return CELL_SPURS_JOB_ERROR_FAULT;
            int rc = run_job(jc, (u32)cmd, jc->descriptor_size,
                             acquired_descriptor);
            if (rc) return rc;
            pc += 8; jobchain_set_current(jc, pc); continue;
        }
        if (cmd == 0 || op == 2 || op == 5) {
            pc += 8; jobchain_set_current(jc, pc); continue;
        }
        if (op == 1 || op == 3) {
            u32 target = (u32)(cmd & ~7ull);
            /*
             * A circular job stream publishes unused NEXT slots as a link to
             * the slot itself.  The PPU later replaces the head command.
             * Treat that stable self-link as an empty predicate, not as 65K
             * executable NEXT commands.
             */
            if (op == 3 && target == pc) {
                int rc = wait_job_slot(jc, pc, cmd);
                if (rc) return rc;
                pc = jobchain_get_current(jc);
                continue;
            }
            if (op == 1) jobchain_reset_calls(jc); /* RESET_PC discards CALL state. */
            pc = target; jobchain_set_current(jc, pc); continue;
        }
        if (op == 4) {
            if (!jobchain_push_call(jc, pc + 8))
                return CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
            pc = (u32)(cmd & ~7ull);
            jobchain_set_current(jc, pc); continue;
        }
        if (op == 6) {
            /* SDK CellSpursJobList: u32 count, u32 descriptor size, u64 EA. */
            u32 list = (u32)(cmd & ~7ull);
            if (list & 0x0fu)
                return CELL_SPURS_JOB_ERROR_JOBLIST_ALIGN;
            if (!job_guest_range_valid(list, 16))
                return CELL_SPURS_JOB_ERROR_FAULT;
            u32 count = rd32(vm_base + list);
            u32 list_descriptor_size = rd32(vm_base + list + 4);
            u32 first = (u32)rd64(vm_base + list + 8);
            if (list_descriptor_size < 0x30 ||
                (list_descriptor_size & 0x0f))
                return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
            const u64 list_bytes = (u64)count * list_descriptor_size;
            if (!job_guest_range_valid(first, list_bytes))
                return CELL_SPURS_JOB_ERROR_FAULT;
            for (u32 i = 0; i < count; ++i) {
                const u32 descriptor_ea =
                    (u32)((u64)first + (u64)i * list_descriptor_size);
                int rc = run_job(jc, descriptor_ea,
                                  list_descriptor_size, NULL);
                if (rc) return rc;
            }
            pc += 8; jobchain_set_current(jc, pc); continue;
        }
        if (op == 7) {
            if (ext == 127) return CELL_OK;                    /* END */
            if (ext == 119) { if (!jobchain_pop_call(jc, &pc)) return CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
                              jobchain_set_current(jc, pc);
                              continue; } /* RET */
            if (ext == 7) return CELL_SPURS_JOB_ERROR_ABORT;   /* ABORT */
            if (ext == 15) {
                int rc = wait_guard(jc, (u32)(cmd & ~127ull));
                if (rc) return rc;
                pc += 8; jobchain_set_current(jc, pc); continue;
            }
            if (ext == 23) {
                pc += 8; jobchain_set_current(jc, pc); continue;
            }                                                   /* SET_LABEL */
        }
        diag("job-command", jc->sync.key, cmd);
        return CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
    }
    return CELL_OK;
}

static void jobchain_reset_run_locked(JobChainState* jc)
{
    jobchain_clear_queued_snapshots(jc);
    jc->pending_snapshot_sequence = 0;
    jc->current_ea = jc->entry_ea;
    jc->call_depth = 0;
    jc->command_snapshot_count = 0;
    jc->command_snapshot_index = 0;
    jc->waiting_command = 0;
    jc->parked_slot_count = 0;
    jc->command_state_count = 0;
    memset(jc->command_state_ea, 0, sizeof(jc->command_state_ea));
    memset(jc->parked_slot_ea, 0, sizeof(jc->parked_slot_ea));
    memset(jc->parked_slot_empty, 0, sizeof(jc->parked_slot_empty));
    for (u32 i = 0; i < MAX_JOB_PARKED_SLOTS; ++i)
        atomic_store_explicit(&jc->command_consumed[i], 0,
                              memory_order_relaxed);
    memset(jc->command_publication_sequence, 0,
           sizeof(jc->command_publication_sequence));
    memset(jc->deferred_resume_sequence, 0,
           sizeof(jc->deferred_resume_sequence));
    memset(jc->deferred_publication_sequence, 0,
           sizeof(jc->deferred_publication_sequence));
    jc->deferred_sequence_next = 0;
    jc->watch_lo = jc->entry_ea;
    jc->watch_hi = jc->entry_ea + 8u;
    jc->alternate_slot = 0x4c00;
    jc->next_dma_tag = 0;
    jobchain_watch_ea_locked(jc, jc->entry_ea);
}
static void jobchain_run(JobChainState* jc)
{
#if defined(YZ_PERF_PROFILE) && defined(_WIN32)
    fprintf(stderr,
            "[native-spurs-profile] jobchain=0x%08X host_tid=%lu\n",
            guest_ea(jc->sync.key), GetCurrentThreadId());
#endif
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] jobchain-start entry=0x%08X\n",
                jc->current_ea);
    int rc = execute_chain(jc);
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] jobchain-finish pc=0x%08X rc=0x%08X\n",
                jc->current_ea, (u32)rc);
    mx_lock(&jc->sync.mutex);
    jc->error = rc;
    jc->running = 0;
    jc->complete = 1;
    spurs_set_workload_runnable(jc->spurs, jc->wid, 0);
    cv_wake_all(&jc->sync.cond);
    mx_unlock(&jc->sync.mutex);
}
#if defined(_WIN32)
static DWORD WINAPI job_thread_proc(LPVOID p) { jobchain_run((JobChainState*)p); return 0; }
#else
static void* job_thread_proc(void* p) { jobchain_run((JobChainState*)p); return NULL; }
#endif
s32 cellSpursRunJobChain(const CellSpursJobChain* object)
{
    JobChainState* jc = jobchain_find(object);
    if (!jc) return object ? CELL_SPURS_JOB_ERROR_STAT : CELL_SPURS_JOB_ERROR_NULL_POINTER;
    mx_lock(&jc->sync.mutex);
    if (jc->running) { mx_unlock(&jc->sync.mutex); return CELL_SPURS_JOB_ERROR_BUSY; }
    if (jc->thread_valid && !jc->joined) {
#if defined(_WIN32)
        WaitForSingleObject(jc->thread, INFINITE);
        CloseHandle(jc->thread);
#else
        pthread_join(jc->thread, NULL);
#endif
        jc->joined = 1;
        jc->thread_valid = 0;
    }
    jobchain_reset_run_locked(jc);
    jc->running = 1;
    jc->complete = 0;
    jc->error = 0;
    jc->joined = 0;
    atomic_store(&jc->shutdown, 0);
    jc->thread_valid = nthread_create_spu(&jc->thread, job_thread_proc, jc);
    if (!jc->thread_valid) { jc->running = 0; mx_unlock(&jc->sync.mutex); return CELL_SPURS_JOB_ERROR_NOMEM; }
    spurs_set_workload_runnable(jc->spurs, jc->wid, 1);
    mx_unlock(&jc->sync.mutex);
    return CELL_OK;
}
s32 cellSpursShutdownJobChain(const CellSpursJobChain* object)
{
    JobChainState* jc = jobchain_find(object);
    if (!jc) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&jc->sync.mutex);
    atomic_store(&jc->shutdown, 1);
    spurs_set_workload_runnable(jc->spurs, jc->wid, 0);
    cv_wake_all(&jc->sync.cond);
    mx_unlock(&jc->sync.mutex);
    for (u32 i = 0; i < MAX_JOB_GUARDS; ++i) {
        if (g_jobguards[i].sync.live && g_jobguards[i].chain == jc) {
            mx_lock(&g_jobguards[i].sync.mutex);
            cv_wake_all(&g_jobguards[i].sync.cond);
            mx_unlock(&g_jobguards[i].sync.mutex);
        }
    }
    return CELL_OK;
}
s32 cellSpursJoinJobChain(CellSpursJobChain* object)
{
    JobChainState* jc = jobchain_find(object);
    if (!jc) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&jc->sync.mutex);
    while (!jc->complete) cv_wait(&jc->sync.cond, &jc->sync.mutex);
    int rc = jc->error;
    if (jc->thread_valid && !jc->joined) {
#if defined(_WIN32)
        WaitForSingleObject(jc->thread, INFINITE); CloseHandle(jc->thread);
#else
        pthread_join(jc->thread, NULL);
#endif
        jc->joined = 1;
        jc->thread_valid = 0;
    }
    jobchain_clear_queued_snapshots(jc);
    mx_unlock(&jc->sync.mutex);
    return rc;
}
s32 cellSpursJobGuardInitialize(const CellSpursJobChain* chain,
                                 CellSpursJobGuard* object, u32 count,
                                 u8 request, u8 auto_reset)
{
    if (!chain || !object) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(object, 128)) return CELL_SPURS_JOB_ERROR_ALIGN;
    JobChainState* jc = jobchain_find(chain);
    if (!jc) return CELL_SPURS_JOB_ERROR_INVAL;
    JobGuardState* g = jobguard_make(object);
    if (!g) return CELL_SPURS_JOB_ERROR_NOMEM;
    g->chain = jc; g->count = g->original = count;
    g->request_count = request; g->auto_reset = auto_reset;
    memset(object->bytes, 0, 128);
    wr32(object->bytes + 0x00, count);
    wr32(object->bytes + 0x04, count);
    wr64(object->bytes + 0x08, guest_ea(chain));
    object->bytes[0x13] = request;
    object->bytes[0x23] = auto_reset;
    return CELL_OK;
}
s32 cellSpursJobGuardNotify(CellSpursJobGuard* object)
{
    if (!object) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(object, 128)) return CELL_SPURS_JOB_ERROR_ALIGN;
    JobGuardState* g = jobguard_find(object);
    if (!g) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&g->sync.mutex);
    if (!g->count) { mx_unlock(&g->sync.mutex); return CELL_SPURS_JOB_ERROR_STAT; }
    --g->count;
    job_publish_u32(guest_ea(object), g->count);
    int released = !g->count;
    if (released) cv_wake_all(&g->sync.cond);
    mx_unlock(&g->sync.mutex);
    return CELL_OK;
}

s32 cellSpursJobGuardReset(CellSpursJobGuard* object)
{
    if (!object) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(object, 128)) return CELL_SPURS_JOB_ERROR_ALIGN;
    JobGuardState* g = jobguard_find(object);
    if (!g) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&g->sync.mutex);
    g->count = g->original;
    job_publish_u32(guest_ea(object), g->count);
    mx_unlock(&g->sync.mutex);
    return CELL_OK;
}
