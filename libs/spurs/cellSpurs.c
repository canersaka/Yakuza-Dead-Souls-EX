/*
 * Firmware-free, host-native SPURS scheduler.
 *
 * The implementation deliberately keeps guest object bytes authoritative.
 * Host locks, condition variables and workers are held in bounded side tables
 * keyed by the guest object pointer.
 */
#include "cellSpurs.h"
#include "../../runtime/spu/spu_workload.h"
#include "../../runtime/memory/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

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
static int aligned(const void* p, uintptr_t n) { return ((uintptr_t)p & (n - 1)) == 0; }
static void diag(const char* kind, const void* object, u64 value)
{
    fprintf(stderr, "[native-spurs] divergence=%s object=0x%08X value=0x%llX\n",
            kind, guest_ea(object), (unsigned long long)value);
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
    int shutdown;
} SpursState;
static SpursState g_spurs[MAX_SPURS_INSTANCES];

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
    int waiting;
    int signalled;
    int exit_code;
} TaskState;

struct TasksetState {
    SyncKey sync;
    void* spurs;
    u64 args;
    u32 size;
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
    if (op == 0) {
        t->exit_code = (s32)ctx->gpr[4]._u32[0];
        return -1;
    }
    if (op == 1 || op == 2) {
        task_save_context(t, ctx);
        mx_lock(&ts->sync.mutex);
        if (op == 2) {
            t->waiting = 1;
            task_bit(ts->sync.key, 0x50, t->id, 1);
            while (!t->signalled && !ts->shutdown)
                cv_wait(&ts->sync.cond, &ts->sync.mutex);
            t->signalled = 0;
            task_bit(ts->sync.key, 0x40, t->id, 0);
            task_bit(ts->sync.key, 0x50, t->id, 0);
            t->waiting = 0;
        }
        mx_unlock(&ts->sync.mutex);
        task_restore_context(t, ctx);
        ctx->gpr[3] = spu_make_preferred_u32(ts->shutdown ?
                                             (u32)CELL_SPURS_TASK_ERROR_SHUTDOWN : 0);
        return ts->shutdown ? 0 : 1;
    }
    if (op == 3) {
        const u8* object = (const u8*)ts->sync.key;
        int found_task = 0;
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
        ctx->gpr[3] = spu_make_preferred_u32(found_task ? 1u : 0u);
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
    spu_context* ctx = (spu_context*)malloc(sizeof(*ctx));
    if (!ctx) {
        task->exit_code = CELL_SPURS_TASK_ERROR_NOMEM;
        goto finished;
    }
    spu_context_init(ctx, task->id);
    u32 entry = 0;
    if (!spu_elf_load_to_ls(task->elf, task->elf_size, ctx->ls, &entry)) {
        task->exit_code = CELL_SPURS_TASK_ERROR_NOEXEC;
        free(ctx);
        goto finished;
    }
    u8* stc = ctx->ls + 0x2700;
    memcpy(stc, task->owner->sync.key, 128);
    memcpy(ctx->ls + 0x2780, task->argument, 16);
    wr64(ctx->ls + 0x2790, guest_ea(task->elf));
    wr64(ctx->ls + 0x2798, task->context_ea);
    memcpy(ctx->ls + 0x27a0, task->ls_pattern, 16);
    wr32(ctx->ls + 0x27b8, guest_ea(task->owner->sync.key));
    wr32(ctx->ls + 0x27bc, guest_ea(task->owner->sync.key));
    wr32(ctx->ls + 0x27c4, 0x0a70);
    wr32(ctx->ls + 0x27d4, task->id);
    wr32(ctx->ls + 0x2fb8, 0x2700);
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

    mx_lock(&task->owner->sync.mutex);
    task_bit(task->owner->sync.key, 0x10, task->id, 0);
    task_bit(task->owner->sync.key, 0x00, task->id, 1);
    mx_unlock(&task->owner->sync.mutex);
    if (!spu_workload_execute(&task->image, ctx))
        task->exit_code = CELL_SPURS_TASK_ERROR_NOEXEC;
    free(ctx);
finished:
    mx_lock(&task->owner->sync.mutex);
    task_bit(task->owner->sync.key, 0x00, task->id, 0);
    task_bit(task->owner->sync.key, 0x30, task->id, 0);
    task->complete = 1;
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
static void task_join_thread(TaskState* t)
{
    if (!t->thread_valid || t->joined) return;
#if defined(_WIN32)
    WaitForSingleObject(t->thread, INFINITE);
    CloseHandle(t->thread);
#else
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
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
                                 u64 args)
{
    if (!spurs || !taskset) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!aligned(taskset, 128)) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!spurs_find(spurs)) return CELL_SPURS_TASK_ERROR_STAT;
    TasksetState* ts = taskset_make(taskset);
    if (!ts) return CELL_SPURS_TASK_ERROR_NOMEM;
    memset(taskset, 0, size);
    ts->spurs = spurs;
    ts->args = args;
    ts->size = size;
    ts->shutdown = 0;
    memset(ts->tasks, 0, sizeof(ts->tasks));
    wr64((u8*)taskset + 0x60, guest_ea(spurs));
    wr64((u8*)taskset + 0x68, args);
    return CELL_OK;
}
s32 cellSpursCreateTaskset(CellSpurs* s, CellSpursTaskset* ts, u64 args,
                           const u8* priority, u32 max)
{
    (void)priority; (void)max;
    return create_taskset_common(s, ts, sizeof(*ts), args);
}
s32 cellSpursCreateTasksetWithAttribute(CellSpurs* s, CellSpursTaskset* ts,
                                        const CellSpursTasksetAttribute* a)
{
    if (!a) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    return create_taskset_common(s, ts, rd32(a->bytes + 0x20),
                                 rd64(a->bytes + 0x08));
}
s32 cellSpursCreateTaskset2(CellSpurs* s, CellSpursTaskset2* ts,
                            const CellSpursTasksetAttribute2* a)
{
    u64 args = a ? rd64(a->bytes + 0x08) : 0;
    return create_taskset_common(s, ts, sizeof(*ts), args);
}
s32 cellSpursShutdownTaskset(CellSpursTaskset* object)
{
    if (!object) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    TasksetState* ts = taskset_find(object);
    if (!ts) return CELL_SPURS_TASK_ERROR_STAT;
    mx_lock(&ts->sync.mutex);
    ts->shutdown = 1;
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
    for (u32 i = 0; i < 128; ++i) task_join_thread(&ts->tasks[i]);
    return CELL_OK;
}
s32 cellSpursDestroyTaskset(CellSpursTaskset* ts)
{
    s32 rc = cellSpursShutdownTaskset(ts);
    return rc ? rc : cellSpursJoinTaskset(ts);
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
    if (id >= 128 || !ts->tasks[id].thread_valid) return CELL_SPURS_TASK_ERROR_NOENT;
    TaskState* task = &ts->tasks[id];
    mx_lock(&ts->sync.mutex);
    if (try_only && !task->complete) {
        mx_unlock(&ts->sync.mutex);
        return CELL_SPURS_TASK_ERROR_BUSY;
    }
    while (!task->complete) cv_wait(&ts->sync.cond, &ts->sync.mutex);
    s32 code = task->exit_code;
    mx_unlock(&ts->sync.mutex);
    task_join_thread(task);
    if (exit) wr32(exit, (u32)code);
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

void cellSpursNotifyGuestWrite(u32 ea, u32 size)
{
    if (g_registry_ready != 2 || !vm_base || !size) return;
    const u64 first = ea;
    const u64 last = first + size;
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
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    ef->bytes[0x0c] = 0x10;
    wr32(ef->bytes + 0x78, 1);
    wr32(ef->bytes + 0x7c, 1);
    return CELL_OK;
}
s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* ef)
{
    if (!ef) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    ef->bytes[0x0c] = 0xff;
    wr32(ef->bytes + 0x78, 0);
    wr32(ef->bytes + 0x7c, 0);
    return CELL_OK;
}
s32 cellSpursEventFlagSet(CellSpursEventFlag* ef, u16 bits)
{
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return ef ? CELL_SPURS_TASK_ERROR_STAT : CELL_SPURS_TASK_ERROR_NULL_POINTER;
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
        u32 ts_ea = (u32)rd64(ef->bytes + 0x70);
        if (vm_base && ts_ea)
            _cellSpursSendSignal((CellSpursTaskset*)(vm_base + ts_ea), ef->bytes[0x50 + slot]);
    }
    cv_wake_all(&sync->cond);
    mx_unlock(&sync->mutex);
    return CELL_OK;
}
static s32 event_wait(CellSpursEventFlag* ef, u16* bits, u32 mode, int try_only)
{
    if (!ef || !bits) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (mode > 1) return CELL_SPURS_TASK_ERROR_INVAL;
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 0);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;
    const u16 mask = rd16(bits);
    mx_lock(&sync->mutex);
    while (!event_ready(rd16(ef->bytes), mask, mode)) {
        if (try_only) { mx_unlock(&sync->mutex); return CELL_SPURS_TASK_ERROR_BUSY; }
        wr16(ef->bytes + 0x04, mask);
        ef->bytes[0x0a] = (u8)mode;
        ef->bytes[0x07] = 1;
        cv_wait(&sync->cond, &sync->mutex);
    }
    u16 got = (u16)(rd16(ef->bytes) & mask);
    wr16(bits, got);
    if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        wr16(ef->bytes, (u16)(rd16(ef->bytes) & ~got));
    ef->bytes[0x07] = 0;
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
    int kind;
} QueueState;
static QueueState g_queues[MAX_QUEUES];

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

typedef struct JobChainState {
    SyncKey sync;
    u32 entry_ea, current_ea;
    u16 descriptor_size;
    u16 max_grab;
    int running, complete, error;
    _Atomic int shutdown;
    u32 alternate_slot;
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

void cellSpursNotifyPpuGuestWrite(u32 ea, u32 size)
{
    cellSpursNotifyGuestWrite(ea, size);
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        JobChainState* jc = &g_jobchains[i];
        if (!jc->sync.live) continue;
        const u64 chain_first = jc->entry_ea;
        const u64 chain_last = chain_first + 4096;
        if (first >= chain_last || last <= chain_first) continue;
        mx_lock(&jc->sync.mutex);
        cv_wake_all(&jc->sync.cond);
        mx_unlock(&jc->sync.mutex);
    }
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
    if (size_desc < 0x30 || (size_desc & 0x0f)) return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
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
    if (!spurs_find(s)) return CELL_SPURS_JOB_ERROR_STAT;
    JobChainState* jc = jobchain_make(object);
    if (!jc) return CELL_SPURS_JOB_ERROR_NOMEM;
    memset(object->bytes, 0, sizeof(object->bytes));
    jc->entry_ea = rd32(a->bytes + 0x08);
    jc->descriptor_size = rd16(a->bytes + 0x0c);
    jc->max_grab = rd16(a->bytes + 0x0e);
    jc->current_ea = jc->entry_ea;
    jc->complete = jc->running = jc->error = 0;
    atomic_store(&jc->shutdown, 0);
    jc->alternate_slot = 0x4c00;
    wr64(object->bytes + 0x00, guest_ea(s));
    wr64(object->bytes + 0x20, jc->entry_ea);
    if (jc->entry_ea < g_native_spurs_ppu_watch_lo)
        g_native_spurs_ppu_watch_lo = jc->entry_ea;
    if (jc->entry_ea + 4096 > g_native_spurs_ppu_watch_hi)
        g_native_spurs_ppu_watch_hi = jc->entry_ea + 4096;
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

static void job_publish_copy(u32 ea, const void* source, u32 size)
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
    cellSpursNotifyGuestWrite(ea, size);
}

static int job_return_syscall(spu_context* ctx, void* opaque)
{
    (void)ctx;
    (void)opaque;
    return -1;
}

static int run_job(JobChainState* jc, u32 descriptor_ea, u32 descriptor_size)
{
    if (!vm_base || descriptor_ea > 0xfffffff0u) return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    const u8* d = vm_base + descriptor_ea;
    u32 bin_ea = (u32)(rd64(d + 0x00) & ~7ull);
    u32 bin_size = (u32)rd16(d + 0x08) * 16;
    if (!bin_ea || !bin_size) return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    if (descriptor_size < 0x30 || descriptor_size > 0x400 ||
        (descriptor_size & 0x0f)) return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
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
    if (bin_size > SPU_LS_SIZE - slot) { free(ctx); return CELL_SPURS_JOB_ERROR_INVALID_BIN; }
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
        u32 item_size = (u32)(item >> 32);
        cache_ls[i] = job_alloc(&cursor, descriptor_ls, item_size, 1024);
        if (item_size && !cache_ls[i]) { free(ctx); return CELL_SPURS_JOB_ERROR_NOMEM; }
        if (item_size) memcpy(ctx->ls + cache_ls[i], vm_base + (u32)item, item_size);
    }
    const u32 scratch_stack_ls =
        job_alloc(&cursor, descriptor_ls, size_scratch + size_stack, 1024);
    if ((size_io && !io_ls) || (size_out && !out_ls) || !scratch_stack_ls) {
        free(ctx);
        return CELL_SPURS_JOB_ERROR_NOMEM;
    }

    u32 io_offset = 0;
    const u32 io_count = dma_list_size / 8;
    for (u32 i = 0; i < io_count; ++i) {
        u64 item = rd64(d + 0x30 + i * 8);
        u32 item_size = (u32)(item >> 32) & 0x7fffu;
        u32 item_ea = (u32)item;
        if (item_size > 0x4000 || io_offset + item_size > size_io ||
            (item_size && !item_ea)) {
            free(ctx);
            return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
        }
        if (item_size) memcpy(ctx->ls + io_ls + io_offset,
                              vm_base + item_ea, item_size);
        io_offset = (io_offset + item_size + 15u) & ~15u;
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
    wr32(ctx->ls + context_ls + 0x24, 20);
    wr64(ctx->ls + context_ls + 0x28, descriptor_ea);

    ctx->gpr[0]._u32[0] = 0x0a70;
    ctx->gpr[1]._u32[0] = scratch_stack_ls + size_scratch + size_stack;
    ctx->gpr[3]._u32[0] = context_ls;
    ctx->gpr[4]._u32[0] = descriptor_ls;
    ctx->native_spurs_syscall = job_return_syscall;
    ctx->pc = slot;
    image.entry_pc = slot;
    int ok = spu_workload_execute(&image, ctx);

    if (ok && rd32(d + 0x10)) {
        io_offset = 0;
        for (u32 i = 0; i < io_count; ++i) {
            u64 item = rd64(d + 0x30 + i * 8);
            u32 item_size = (u32)(item >> 32) & 0x7fffu;
            if (item_size)
                job_publish_copy((u32)item, ctx->ls + io_ls + io_offset, item_size);
            io_offset = (io_offset + item_size + 15u) & ~15u;
        }
    }
    free(ctx);
    return ok ? CELL_OK : CELL_SPURS_JOB_ERROR_NOEXEC;
}
static int wait_guard(JobChainState* jc, u32 guard_ea)
{
    if (!vm_base) return CELL_SPURS_JOB_ERROR_INVAL;
    JobGuardState* g = jobguard_find(vm_base + guard_ea);
    if (!g) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&g->sync.mutex);
    while (g->count && !atomic_load(&jc->shutdown))
        cv_wait(&g->sync.cond, &g->sync.mutex);
    if (!g->count && g->auto_reset) {
        g->count = g->original;
        wr32(g->sync.key, g->count);
    }
    mx_unlock(&g->sync.mutex);
    return atomic_load(&jc->shutdown) ? CELL_SPURS_JOB_ERROR_ABORT : CELL_OK;
}
static int wait_job_slot(JobChainState* jc, u32 pc, u64 empty_command)
{
    jc->current_ea = pc;
    mx_lock(&jc->sync.mutex);
    while (rd64(vm_base + pc) == empty_command &&
           !atomic_load(&jc->shutdown))
        cv_wait(&jc->sync.cond, &jc->sync.mutex);
    mx_unlock(&jc->sync.mutex);
    return atomic_load(&jc->shutdown) ?
           CELL_SPURS_JOB_ERROR_ABORT : CELL_OK;
}
static int execute_chain(JobChainState* jc)
{
    if (!vm_base || !jc->current_ea) return CELL_SPURS_JOB_ERROR_INVAL;
    u32 pc = jc->current_ea, stack[16], depth = 0;
    for (u32 step = 0; step < 65536 && !atomic_load(&jc->shutdown); ++step) {
        u64 cmd = rd64(vm_base + pc);
        u32 op = (u32)(cmd & 7), ext = (u32)(cmd & 127);
        if (cmd == 0x0000000800000012ull) {
            int rc = wait_job_slot(jc, pc, cmd);
            if (rc) return rc;
            continue;
        }
        if (cmd && op == 0) {
            int rc = run_job(jc, (u32)cmd, jc->descriptor_size);
            if (rc) return rc;
            pc += 8; jc->current_ea = pc; continue;
        }
        if (cmd == 0 || op == 2 || op == 5) {
            pc += 8; jc->current_ea = pc; continue;
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
                continue;
            }
            if (op == 1) depth = 0; /* RESET_PC also discards CALL state. */
            pc = target; jc->current_ea = pc; continue;
        }
        if (op == 4) {
            if (depth == 16) return CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
            stack[depth++] = pc + 8; pc = (u32)(cmd & ~7ull);
            jc->current_ea = pc; continue;
        }
        if (op == 6) {
            /* SDK CellSpursJobList: u32 count, u32 descriptor size, u64 EA. */
            u32 list = (u32)(cmd & ~7ull);
            u32 count = rd32(vm_base + list);
            u32 list_descriptor_size = rd32(vm_base + list + 4);
            u32 first = (u32)rd64(vm_base + list + 8);
            if (list_descriptor_size < 0x30 ||
                (list_descriptor_size & 0x0f))
                return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
            for (u32 i = 0; i < count; ++i) {
                int rc = run_job(jc, first + i * list_descriptor_size,
                                 list_descriptor_size);
                if (rc) return rc;
            }
            pc += 8; jc->current_ea = pc; continue;
        }
        if (op == 7) {
            if (ext == 127) return CELL_OK;                    /* END */
            if (ext == 119) { if (!depth) return CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
                              pc = stack[--depth]; jc->current_ea = pc;
                              continue; } /* RET */
            if (ext == 7) return CELL_SPURS_JOB_ERROR_ABORT;   /* ABORT */
            if (ext == 15) {
                int rc = wait_guard(jc, (u32)(cmd & ~127ull));
                if (rc) return rc;
                pc += 8; jc->current_ea = pc; continue;
            }
            if (ext == 23) {
                pc += 8; jc->current_ea = pc; continue;
            }                                                   /* SET_LABEL */
        }
        diag("job-command", jc->sync.key, cmd);
        return CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
    }
    return atomic_load(&jc->shutdown) ?
           CELL_SPURS_JOB_ERROR_ABORT : CELL_SPURS_JOB_ERROR_UNKNOWN_CMD;
}
static void jobchain_run(JobChainState* jc)
{
    int rc = execute_chain(jc);
    mx_lock(&jc->sync.mutex);
    jc->error = rc;
    jc->running = 0;
    jc->complete = 1;
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
    jc->running = 1;
    jc->complete = 0;
    jc->error = 0;
    jc->joined = 0;
    atomic_store(&jc->shutdown, 0);
    jc->thread_valid = nthread_create_spu(&jc->thread, job_thread_proc, jc);
    if (!jc->thread_valid) { jc->running = 0; mx_unlock(&jc->sync.mutex); return CELL_SPURS_JOB_ERROR_NOMEM; }
    mx_unlock(&jc->sync.mutex);
    return CELL_OK;
}
s32 cellSpursShutdownJobChain(const CellSpursJobChain* object)
{
    JobChainState* jc = jobchain_find(object);
    if (!jc) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&jc->sync.mutex);
    atomic_store(&jc->shutdown, 1);
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
    mx_unlock(&jc->sync.mutex);
    return rc;
}
s32 cellSpursJobGuardInitialize(const CellSpursJobChain* chain,
                                CellSpursJobGuard* object, u32 count,
                                u8 request, u8 auto_reset)
{
    JobChainState* jc = jobchain_find(chain);
    if (!jc || !object) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(object, 128)) return CELL_SPURS_JOB_ERROR_ALIGN;
    if (!count) return CELL_SPURS_JOB_ERROR_INVAL;
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
    JobGuardState* g = jobguard_find(object);
    if (!g) return object ? CELL_SPURS_JOB_ERROR_STAT : CELL_SPURS_JOB_ERROR_NULL_POINTER;
    mx_lock(&g->sync.mutex);
    if (!g->count) { mx_unlock(&g->sync.mutex); return CELL_SPURS_JOB_ERROR_STAT; }
    --g->count; wr32(object->bytes, g->count);
    int released = !g->count;
    if (released) cv_wake_all(&g->sync.cond);
    mx_unlock(&g->sync.mutex);
    if (released)
        (void)cellSpursRunJobChain((const CellSpursJobChain*)g->chain->sync.key);
    return CELL_OK;
}
