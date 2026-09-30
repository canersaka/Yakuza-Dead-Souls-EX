/* Tasksets and task execution.
 * Part of cellSpurs.c, which includes its parts once, in order. */

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
    int signalled;
    int exit_code;
    u32 exit_container_ea;   /* guest CellSpursTaskExitCode (0 = none) */
} TaskState;

struct TasksetState {
    SyncKey sync;
    void* spurs;
    u32 wid;
    int wid_registered;
    u8 priority[8];
    int shutdown;
    TaskState tasks[128];
};
static TasksetState g_tasksets[MAX_TASKSETS];
static void event_observe_native_wait(TasksetState* ts, TaskState* task,
                                      u32 candidate_ea);


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

extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
extern int spu_coh_is_reserved(u32);
extern void spu_coh_notify_write(u32);

static void task_bit(void* ts, u32 off, u32 id, int value)
{
    u8* p = (u8*)ts + off + id / 8;
    u8 mask = (u8)(0x80u >> (id & 7));
    const u32 ea = guest_ea(p);
    /* The taskset header is also mutated by lifted SPU GETLLAR/PUTLLC
     * transactions.  A raw host byte store to its bitsets while an SPU owns a
     * reservation on the same 128-byte line would let a later successful
     * full-line PUTLLC restore an old running/ready/waiting/signal byte and
     * lose a barrier wake.  Taskset lines become permanently marked by the
     * first GETLLAR,
     * so the ordinary initialization path remains lock-free while every
     * concurrent native transition serializes with the SPU transaction and
     * publishes reservation loss. */
    if (spu_coh_is_reserved(ea)) {
        spu_lockline_lock();
        if (value) *p |= mask; else *p &= (u8)~mask;
        spu_coh_notify_write(ea);
        spu_lockline_unlock();
    } else {
        if (value) *p |= mask; else *p &= (u8)~mask;
    }
}

#if defined(YZ_SPURS_TEST_GUEST_SIZE)
void cellSpursTestTaskBit(void* taskset, u32 offset, u32 id, int value)
{
    task_bit(taskset, offset, id, value);
}
#endif
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
static void task_save_context(TaskState* t, spu_context* ctx, int retain_ls)
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
    if (retain_ls) return;
    u32 cursor = 1024;
    for (u32 block = 0; block < 128 && cursor + 2048 <= t->context_size; ++block) {
        if (t->ls_pattern[block / 8] & (0x80u >> (block & 7))) {
            memcpy(out + cursor, ctx->ls + block * 2048, 2048);
            cursor += 2048;
        }
    }
}
static atomic_int g_gs_yield_mode;
atomic_ullong g_yz_gs_poll_yield_calls;
u64 cellSpursGetGsPollYieldCalls(void)
{
    return atomic_load_explicit(&g_yz_gs_poll_yield_calls, memory_order_relaxed);
}
static void gs_poll_yield(void)
{
    atomic_fetch_add_explicit(&g_yz_gs_poll_yield_calls, 1, memory_order_relaxed);
#if defined(_WIN32)
    Sleep(0);
#else
    nthread_yield();
#endif
}
int cellSpursSetGsYieldMode(int enabled)
{
    atomic_store_explicit(&g_gs_yield_mode, !!enabled, memory_order_relaxed);
    return !!enabled;
}
static void task_restore_context(TaskState* t, spu_context* ctx, int retain_ls)
{
    if (!t->context_ea || t->context_size < 1024 || !vm_base) return;
    const u8* in = vm_base + t->context_ea;
    ctx->gpr[0] = task_load_qword(in + 0x000);
    ctx->gpr[1] = task_load_qword(in + 0x010);
    for (u32 reg = 80; reg < 128; ++reg)
        ctx->gpr[reg] = task_load_qword(in + 0x020 + (reg - 80) * 16);
    ctx->fpscr = task_load_qword(in + 0x320);
    ctx->event_mask = rd32(in + 0x334);
    if (retain_ls) return;
    u32 cursor = 1024;
    for (u32 block = 0; block < 128 && cursor + 2048 <= t->context_size; ++block) {
        if (t->ls_pattern[block / 8] & (0x80u >> (block & 7))) {
            memcpy(ctx->ls + block * 2048, in + cursor, 2048);
            cursor += 2048;
        }
    }
}
static int gs_idle_wait(spu_context* ctx)
{
#if defined(_WIN32)
    const u32 ea=gs_idle_journal_ready_word(ctx->ls,SPU_LS_SIZE);
    if(!ea||!vm_base||!job_guest_range_valid(ea,4))return 0;
    /* The PPU filters writes by these page bits before calling the common
     * publication hook. Installing only the exact waiter misses ordinary
     * journal stores on pages with no other SPURS synchronization object. */
    const u32 page=ea>>GUEST_WRITE_PAGE_SHIFT;
    if(!(guest_write_route_page_word(page>>6)&(1ull<<(page&63))))
        guest_write_route_mark_page(ea);
    volatile u32* word=(volatile u32*)(vm_base+ea);
    atomic_store_explicit(&g_gs_idle_wait_word,ea,memory_order_release);
#if defined(YZ_SPURS_TEST_GUEST_SIZE)
    extern void yz_spurs_test_gs_idle_before_wait(u32);
    yz_spurs_test_gs_idle_before_wait(ea);
#endif
    const u32 zero=0;
    /* Waiting on the authoritative word closes publication-before-wait races:
     * a tag published before registration or before WaitOnAddress differs from
     * zero and prevents the park. Notifications are hints only. A bounded
     * recheck still covers legacy writers which bypass the VM-write hook. */
    int waited=0;
    if(!*word) {
        waited=1;
        WaitOnAddress(word,(void*)&zero,sizeof(zero),1);
    }
    atomic_store_explicit(&g_gs_idle_wait_word,0,memory_order_release);
    return waited;
#else
    (void)ctx;return 0;
#endif
}
#if defined(YZ_SPURS_TEST_GUEST_SIZE)
int cellSpursTestGsIdleWait(spu_context* ctx)
{
    return gs_idle_wait(ctx);
}
#endif
static int task_syscall(spu_context* ctx, void* opaque)
{
    TaskState* t = (TaskState*)opaque;
    TasksetState* ts = t->owner;
    const u32 op = ctx->gpr[3]._u32[0] & 0x0f;
    if (op == 0) {
        /* The task exit wrapper saves its exit code in the taskset context
         * before it reuses r4 to locate the syscall entry.  Reading r4 here
         * reports that internal LS address (usually 0x27c4), not the value
         * supplied to cellSpursExit. */
        t->exit_code = (s32)rd32(ctx->ls + 0x2fd0);
        return -1;
    }
    if (op == 1 || op == 2) {
        const u32 handoff_pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        /* task_run owns one context for its whole host-thread lifetime. This
         * exact GS polling YIELD resumes on the same stack and does not lend
         * LS to another task. The MFC switch below only clears reservations.
         * Retain the private LS instead of emulating a hardware task swap.
         * Keep the public register header and all scheduler transitions; real
         * signal waits and every other task still export/restore the full LS. */
        const int retain_ls=op==1 && t->image.image_id==0 && handoff_pc==0x7c0cu;
        /* Both scheduler handoffs save LS/context for another dispatch.
         * Complete all task DMA first so WAIT_SIGNAL cannot expose a partial
         * context any more than YIELD can. */
        spu_task_wait_all_dma(ctx);
        if (op == 2) {
            const u32 event_eah = ctx->gpr[81]._u32[0];
            const u32 event_eal = ctx->gpr[81]._u32[1];
            /* A real lifted EventFlagWait has only the MFC address pair in
             * r81.  Keep discovery for that ABI, but never reinterpret the
             * barrier-plus-context tuple as an event flag. */
            if (event_eah == 0 && ctx->gpr[81]._u32[2] == 0u &&
                ctx->gpr[81]._u32[3] == 0u)
                event_observe_native_wait(ts, t, event_eal);
        }
        task_save_context(t, ctx, retain_ls);
        spu_mfc_context_switch(ctx);
        mx_lock(&ts->sync.mutex);
        if (op == 1) {
            /* YIELD returns the task to READY while its host worker gives the
             * scheduler a chance to run another native workload. */
            task_bit(ts->sync.key, 0x00, t->id, 0);
            taskset_refresh_runnable_locked(ts);
            mx_unlock(&ts->sync.mutex);
            /* A task yield is a scheduler transition: the context save and
             * READY transition are complete, and guest state is rechecked when
             * the task resumes. The GS polling yield parks on its journal word
             * for at most 1 ms (or, with the yield mode set, just gives up the
             * host quantum); every other yield and publication retry backs off
             * for 1 ms before the task is selected again. */
            if (t->image.image_id == 0 && handoff_pc == 0x7c0cu && gs_idle_wait(ctx)) {
                /* Resume the original loop: this wait publishes no completion. */
            } else if (t->image.image_id == 0 && handoff_pc == 0x7c0cu &&
                atomic_load_explicit(&g_gs_yield_mode, memory_order_relaxed)) {
                gs_poll_yield();
            } else {
                nthread_reschedule();
            }
            mx_lock(&ts->sync.mutex);
            if (!ts->shutdown) {
                task_bit(ts->sync.key, 0x00, t->id, 1);
                taskset_refresh_runnable_locked(ts);
            }
        } else {
            task_bit(ts->sync.key, 0x00, t->id, 0);
            task_bit(ts->sync.key, 0x10, t->id, 0);
            task_bit(ts->sync.key, 0x50, t->id, 1);
            taskset_refresh_runnable_locked(ts);
            /* The signal buffer is a depth-1 latch in the guest bitset
             * (+0x40): a signal sent before the wait is consumed at once.
             * Lifted SPU signalers write only the guest bit and never set
             * the host mirror t->signalled, so the predicate consumes the
             * guest latch too. */
            {
                const u8* obj = (const u8*)ts->sync.key;
                const u8 sig_mask = (u8)(0x80u >> (t->id & 7));
                const u32 sig_byte = 0x40 + t->id / 8;
                while (!t->signalled && !(obj[sig_byte] & sig_mask) &&
                       !ts->shutdown)
                    /* Timed: lifted-SPU signalers bypass the HLE wake. */
                    cv_wait_ms(&ts->sync.cond, &ts->sync.mutex, 2u);
            }
            t->signalled = 0;
            task_bit(ts->sync.key, 0x40, t->id, 0);
            task_bit(ts->sync.key, 0x50, t->id, 0);
            if (!ts->shutdown) {
                task_bit(ts->sync.key, 0x00, t->id, 1);
                task_bit(ts->sync.key, 0x10, t->id, 1);
            }
            taskset_refresh_runnable_locked(ts);
        }
        mx_unlock(&ts->sync.mutex);
        task_restore_context(t, ctx, retain_ls);
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
        const u32 result = (found_task ? 1u : 0u) |
                           (found_workload ? 2u : 0u);
        if (!result)
            nthread_yield();
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
    if (task->image.image_id == 0) yz_place_critical_thread(1);
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
    if (!spu_workload_execute(&task->image, ctx))
        task->exit_code = CELL_SPURS_TASK_ERROR_NOEXEC;
    native_spu_context_free(ctx);
finished:
    mx_lock(&task->owner->sync.mutex);
    task_bit(task->owner->sync.key, 0x00, task->id, 0);
    task_bit(task->owner->sync.key, 0x10, task->id, 0);
    task_bit(task->owner->sync.key, 0x30, task->id, 0);
    task_bit(task->owner->sync.key, 0x40, task->id, 0);
    task_bit(task->owner->sync.key, 0x50, task->id, 0);
    task->complete = 1;
    /* Publish the exit code into the guest exit-code container, value first
     * then the ready flag, so cellSpursTaskExitCode(Try)Get observes a
     * complete record. */
    if (task->exit_container_ea && vm_base) {
        wr32(vm_base + task->exit_container_ea + 4, (u32)task->exit_code);
        vm_base[task->exit_container_ea] = 1;
    }
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
    /* The only legal sizes are the class-0 (0x1900) and class-1 (0x2900)
     * taskset sizes. create_taskset_common memsets the caller's object with
     * this value, so an oversized/garbage attribute word would be an
     * unbounded guest-memory clear. */
    if (size != CELL_SPURS_TASKSET_SIZE && size != CELL_SPURS_TASKSET2_SIZE)
        return CELL_SPURS_TASK_ERROR_INVAL;
    wr32(a->bytes + 0x20, (u32)size);
    return CELL_OK;
}
static s32 create_taskset_common(CellSpurs* spurs, void* taskset, u32 size,
                                  u64 args, const u8* priority,
                                  u8 enable_clear_ls)
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
    ts->wid = wid;
    ts->wid_registered = 1;
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
    {
        const u32 index = (u32)(ts - g_tasksets);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_TASKSET, index);
        /* Lifted tasks publish signals in this 128-bit field.  No other
         * taskset byte is a host-worker wake predicate. */
        guest_write_route_watch(GUEST_WRITE_ROUTE_TASKSET, index,
                                guest_ea(taskset) + 0x40u, 0x10u);
    }
    return CELL_OK;
}
s32 cellSpursCreateTaskset(CellSpurs* s, CellSpursTaskset* ts, u64 args,
                           const u8* priority, u32 max)
{
    (void)max;
    return create_taskset_common(s, ts, sizeof(*ts), args, priority, 0);
}
s32 cellSpursCreateTasksetWithAttribute(CellSpurs* s, CellSpursTaskset* ts,
                                        const CellSpursTasksetAttribute* a)
{
    if (!a) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    /* Validate the size word before create_taskset_common memsets the guest
     * object with it; an uninitialized/foreign attribute otherwise turns
     * into an unbounded guest-memory clear (legal values are exactly the
     * class-0/class-1 sizes). */
    u32 tsize = rd32(a->bytes + 0x20);
    if (tsize != CELL_SPURS_TASKSET_SIZE && tsize != CELL_SPURS_TASKSET2_SIZE)
        return CELL_SPURS_TASK_ERROR_INVAL;
    return create_taskset_common(s, ts, tsize,
                                  rd64(a->bytes + 0x08), a->bytes + 0x10,
                                  a->bytes[0x24] ? 1 : 0);
}
s32 cellSpursCreateTaskset2(CellSpurs* s, CellSpursTaskset2* ts,
                            const CellSpursTasksetAttribute2* a)
{
    u64 args = a ? rd64(a->bytes + 0x08) : 0;
    /* CellSpursTasksetAttribute2 is a transparent struct (revision@0, name@4,
     * argTaskset@8, priority[8]@0x10, maxContention@0x18, enableClearLs
     * int32 @0x1C, taskNameBuffer@0x20), so enableClearLs is read at 0x1C;
     * 0x24 is only right for the opaque class-1 attribute. */
    return create_taskset_common(s, ts, sizeof(*ts), args,
                                  a ? a->bytes + 0x10 : NULL,
                                  (a && rd32(a->bytes + 0x1C) != 0) ? 1 : 0);
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
    /* Join finishes the taskset: release its workload id and recycle the
     * host slot, so MAX_TASKSETS and the workload table are not lifetime
     * totals on the v1 CreateTask+JoinTaskset path. A signal to a joined
     * taskset answers STAT (taskset_find misses), the faithful dead-object
     * reply, instead of latching into whatever recycled the slot.
     * DestroyTaskset's own release tail is then a no-op (its taskset_find
     * misses too). */
    {
        mx_lock(&ts->sync.mutex);
        const int release = ts->wid_registered;
        ts->wid_registered = 0;
        mx_unlock(&ts->sync.mutex);
        if (release)
            spurs_release_workload(spurs_find(ts->spurs), ts->wid);
        registry_init();
        mx_lock(&g_registry_mutex);
        ts->sync.live = 0;
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_TASKSET, (u32)(ts - g_tasksets));
        mx_unlock(&g_registry_mutex);
    }
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
                              const u8 lsp[16], const u8 arg[16],
                              u32 exit_container_ea)
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
    task->exit_container_ea = exit_container_ea;
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
                              arg ? arg->bytes : zero, 0);
}
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset* ts, CellSpursTaskId* out,
                                     const CellSpursTaskAttribute* a)
{
    if (!a || !vm_base) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    u32 elf = (u32)rd64(a->bytes + TA_ELF);
    return create_task_common(ts, out, vm_base + elf,
                              (u32)rd64(a->bytes + TA_CONTEXT),
                              rd32(a->bytes + TA_CONTEXT_SIZE),
                              a->bytes + TA_LSP, a->bytes + TA_ARG,
                              rd32(a->bytes + TA_EXIT));
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
                              arg ? arg->bytes : (const u8[16]){0}, 0);
}
static _Atomic(uintptr_t) g_image4_signal_interceptor;
static _Atomic(void*) g_image4_signal_user;

void cellSpursSetImage4SignalInterceptor(
    CellSpursImage4SignalInterceptor interceptor, void* user)
{
    if (!interceptor) {
        atomic_store_explicit(
            &g_image4_signal_interceptor, (uintptr_t)0,
            memory_order_release);
        atomic_store_explicit(
            &g_image4_signal_user, (void*)0, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&g_image4_signal_user, user,
                          memory_order_relaxed);
    atomic_store_explicit(
        &g_image4_signal_interceptor, (uintptr_t)interceptor,
        memory_order_release);
}

int cellSpursSnapshotImage4Taskset(
    u32 wid, u32 task_ids[5], u32 parameter_eas[5],
    u64* image_fingerprint, u32* image_size, u32* entry_pc, s32* image_id)
{
    if (!task_ids || !parameter_eas || !image_fingerprint || !image_size ||
        !entry_pc || !image_id)
        return 0;
    for (u32 slot = 0; slot < MAX_TASKSETS; ++slot) {
        TasksetState* ts = &g_tasksets[slot];
        if (!ts->sync.live || !ts->wid_registered || ts->wid != wid)
            continue;
        mx_lock(&ts->sync.mutex);
        if (!ts->sync.live || !ts->wid_registered || ts->wid != wid) {
            mx_unlock(&ts->sync.mutex);
            continue;
        }
        u32 count = 0;
        int overflow = 0;
        spu_workload_image identity = {0};
        for (u32 candidate = 0; candidate < CELL_SPURS_MAX_TASK; ++candidate) {
            const TaskState* task = &ts->tasks[candidate];
            if (!task->thread_valid || task->complete)
                continue;
            if (count >= 5u) {
                overflow = 1;
                break;
            }
            if (!count)
                identity = task->image;
            else if (task->image.fingerprint != identity.fingerprint ||
                     task->image.image_size != identity.image_size ||
                     task->image.entry_pc != identity.entry_pc ||
                     task->image.image_id != identity.image_id) {
                overflow = 1;
                break;
            }
            task_ids[count] = candidate;
            parameter_eas[count] = rd32(task->argument);
            ++count;
        }
        if (!overflow && count == 5u) {
            *image_fingerprint = identity.fingerprint;
            *image_size = identity.image_size;
            *entry_pc = identity.entry_pc;
            *image_id = identity.image_id;
            mx_unlock(&ts->sync.mutex);
            return 1;
        }
        mx_unlock(&ts->sync.mutex);
    }
    return 0;
}

/* Caller holds ts->sync.mutex. */
static int image4_intercept_signal_locked(TasksetState* ts, u32 id)
{
    if (!ts || id >= CELL_SPURS_MAX_TASK ||
        ts->wid != 4u)
        return 0;
    const uintptr_t interceptor_bits = atomic_load_explicit(
        &g_image4_signal_interceptor, memory_order_acquire);
    if (!interceptor_bits)
        return 0;
    u32 task_ids[5];
    u32 parameter_eas[5];
    u32 count = 0;
    for (u32 candidate = 0;
         candidate < CELL_SPURS_MAX_TASK && count < 5u; ++candidate) {
        const TaskState* task = &ts->tasks[candidate];
        if (!task->thread_valid || task->complete)
            continue;
        task_ids[count] = candidate;
        parameter_eas[count] = rd32(task->argument);
        ++count;
    }
    return count == 5u &&
        ((CellSpursImage4SignalInterceptor)interceptor_bits)(
            atomic_load_explicit(
                &g_image4_signal_user, memory_order_relaxed),
            ts->wid, id, count, task_ids, parameter_eas,
            ts->tasks[id].image.fingerprint,
            ts->tasks[id].image.image_size,
            ts->tasks[id].image.entry_pc,
            ts->tasks[id].image.image_id);
}

s32 _cellSpursSendSignal(CellSpursTaskset* object, CellSpursTaskId id)
{
    TasksetState* ts = taskset_find(object);
    if (!ts) return CELL_SPURS_TASK_ERROR_STAT;
    if (id >= 128) return CELL_SPURS_TASK_ERROR_SRCH;
    mx_lock(&ts->sync.mutex);
    /* The signal latches into the guest signalled bitset regardless of host
     * thread bookkeeping: a task id snapshotted by a producer must not lose
     * its wake because the host TaskState was recycled in between.  Delivery
     * is still gated on the task existing (guest enabled bit, +0x30); a
     * signal to a dead id returns SRCH instead of latching a phantom wake
     * that a later task would consume in place of its real one. */
    {
        const u8* obj = (const u8*)object;
        if (!(obj[0x30 + id / 8] & (u8)(0x80u >> (id & 7)))) {
            mx_unlock(&ts->sync.mutex);
            return CELL_SPURS_TASK_ERROR_SRCH;
        }
    }
    /* EDGE MLAA prepares all five records before it begins the SendSignal
     * loop. That lets a semantic replacement validate the complete round on
     * the first signal and either consume every exact task or leave every
     * unknown variant on the existing SPURS path. No partial task execution
     * is exposed. The callback performs no scheduler mutation and runs while
     * this taskset is stable under its existing mutex. */
    if (image4_intercept_signal_locked(ts, id)) {
        mx_unlock(&ts->sync.mutex);
        return CELL_OK;
    }
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
        /* "Still running" is AGAIN; BUSY means another thread is already
         * waiting. A caller polling TryJoinTask2 on AGAIN would never
         * terminate against BUSY. */
        return CELL_SPURS_TASK_ERROR_AGAIN;
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
    if (!object->bytes[0]) return CELL_SPURS_TASK_ERROR_AGAIN;
    wr32(out, rd32(object->bytes + 4));
    return CELL_OK;
}
/* Blocking form: polls until the completing task publishes its exit code. */
s32 cellSpursTaskExitCodeGet(CellSpursTaskExitCode* object, s32* out)
{
    if (!object || !out) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    while (!object->bytes[0])
        nthread_yield();
    wr32(out, rd32(object->bytes + 4));
    return CELL_OK;
}
s32 cellSpursTaskGetContextSaveAreaSize(u32* out, const CellSpursTaskLsPattern* lsp)
{
    if (!out || !lsp) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    wr32(out, 1024 + ls_pattern_blocks(lsp->bytes) * 2048);
    return CELL_OK;
}
