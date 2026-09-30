/* Job chain execution, guards and shutdown.
 * Part of cellSpurs.c, which includes its parts once, in order. */

static int run_job(JobChainState* jc, u32 descriptor_ea, u32 descriptor_size,
                   const u8* acquired_descriptor)
{
    return run_job_with_state(jc, descriptor_ea, descriptor_size,
        acquired_descriptor, &jc->alternate_slot, &jc->next_dma_tag, 0);
}

#include "job_parallel_pool.h"

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
    while (g->count && !atomic_load(&jc->shutdown)) {
        /* Re-sync the mirror from guest each wake and wait bounded: a
         * lifted-SPU putllc decrement doesn't signal this condvar, and an
         * untimed wait would turn a lost decrement into a permanent
         * jobchain stall instead of 2 ms latency. */
        const u32 live = rd32((const u8*)g->sync.key + 0x00);
        if (live != g->count) { g->count = live; continue; }
        cv_wait_ms(&g->sync.cond, &g->sync.mutex, 2u);
        g->count = rd32((const u8*)g->sync.key + 0x00);
    }
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
    mx_lock(&jc->sync.mutex);
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
        if (claimed) {
            jc->waiting_command = 0;
            break;
        }
        jc->current_ea = pc;
    }
    if (jc->waiting_command && jobchain_take_queued_snapshot(jc, pc))
        jc->waiting_command = 0;
    else if (jc->waiting_command && rd64(vm_base + pc) != empty_command &&
             jobchain_claim_polled(jc, pc))
        jc->waiting_command = 0;
    const int parked = jc->waiting_command &&
                       rd64(vm_base + pc) == empty_command &&
                       !atomic_load(&jc->shutdown);
    if (parked)
        spurs_set_workload_runnable(jc->spurs, jc->wid, 0);
    /* Treating 0x0000000800000012 as an executable LWSYNC (its low word
     * does decode as 2|(2<<3)) and advancing after a grace does not work:
     * during disc-bound phases the producer's marker->command gap is
     * arbitrarily long, so the scan would race a full lap through marker
     * runs and the pipeline would desync. The marker park below is
     * behaviorally required. */
    while (jc->waiting_command && !atomic_load(&jc->shutdown)) {
        /* A changed word can still be an incomplete narrow-store generation.
         * Keep acquisition and its delayed-notification acknowledgement under
         * this mutex; returning an unclaimed word lets the executor acquire
         * it without recording that the producer's callback is still owed. */
        const u64 live_command = rd64(vm_base + pc);
        if (jobchain_take_queued_snapshot(jc, pc) ||
            (live_command != empty_command && jobchain_claim_polled(jc, pc))) {
            jc->waiting_command = 0;
            break;
        }
        /* Zero retains its ordinary NOP semantics outside a captured prefix. */
        if (!live_command && live_command != empty_command) {
            jc->waiting_command = 0;
            break;
        }
        /* Timed: the producer's append may be an un-notified lifted vector
         * store (see cv_wait_ms). Every wake re-reads the slot. */
        cv_wait_ms(&jc->sync.cond, &jc->sync.mutex, 4u);
        /* Descriptor readiness is not a publication marker: the title writes
         * it before the descriptor body and only publishes that body at a
         * later PPU barrier. An unrestricted parked poll can therefore grab
         * an internally consistent but incomplete descriptor. PPU writes are
         * resumed by cellSpursNotifyPpuFence(); SPU DMA/HLE transfers are
         * resumed by their completion notification. */
    }
    if (parked && !atomic_load(&jc->shutdown))
        spurs_set_workload_runnable(jc->spurs, jc->wid, 1);
    if (jc->waiting_command && !atomic_load(&jc->shutdown)) {
        if (jobchain_take_queued_snapshot(jc, pc) ||
            jobchain_claim_polled(jc, pc))
            jc->waiting_command = 0;
    }
    if (atomic_load(&jc->shutdown))
        jc->waiting_command = 0;
    mx_unlock(&jc->sync.mutex);
    return CELL_OK;
}

/* A CALL command is the publication head for a dynamically assembled
 * sub-list.  Per-store callbacks can schedule the native worker between that
 * head store and the sub-list's final RET store, which is an ordering window
 * real SPURS does not expose.  A zero still has legitimate NOP semantics, so
 * give an active CALL frame a bounded acquisition grace rather than parking
 * forever.  Immutable captures stop before zero; this live read is therefore
 * able to observe the completing RET (or any other tail command). */
static u64 jobchain_acquire_call_command(JobChainState* jc, u32 pc)
{
    for (u32 attempt = 0; attempt < 64u; ++attempt) {
        const u64 first = rd64(vm_base + pc);
        atomic_thread_fence(memory_order_seq_cst);
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
        extern void yz_spurs_call_tail_publication_test_hook(u32, u32);
        yz_spurs_call_tail_publication_test_hook(pc, attempt);
#endif
        const u64 second = rd64(vm_base + pc);
        atomic_thread_fence(memory_order_acquire);
        if (first == second && first != 0u)
            return first;
        if (atomic_load(&jc->shutdown))
            return 0;
        if (attempt < 8u)
            nthread_yield();
        else
            nthread_reschedule();
    }
    return rd64(vm_base + pc);
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
        int acquired_bridge_publication = 0;
        int acquire_call_tail = 0;
        int acquired_live_command = 0;
        u8 parallel_descriptor[0x400];
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
            acquired_bridge_publication =
                jc->command_snapshot[jc->command_snapshot_index]
                    .bridge_publication != 0;
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
                acquired_bridge_publication =
                    jc->command_snapshot[0].bridge_publication != 0;
                jc->command_snapshot_index = 1;
            } else {
                cmd = rd64(vm_base + pc);
                acquired_live_command = 1;
                const int command_slot = jobchain_command_slot(jc, pc, 0);
                if (command_slot >= 0)
                    publication_sequence =
                        jc->command_publication_sequence[command_slot];
            }
        }
        acquire_call_tail = cmd == 0u && jc->call_depth != 0u;
        if (acquire_call_tail) {
            /* Do not hold the chain mutex while allowing the PPU producer to
             * finish publishing the dynamic sub-list. */
            mx_unlock(&jc->sync.mutex);
            cmd = jobchain_acquire_call_command(jc, pc);
            mx_lock(&jc->sync.mutex);
        }
        jobchain_discard_consumed_snapshot(
            jc, pc, publication_sequence);
        jobchain_cancel_deferred_resume(jc, pc, publication_sequence);
        {
            const int is_job = job_command_is_job(cmd);
            const int command_slot = jobchain_command_slot(jc, pc, is_job);
            if (command_slot >= 0) {
                if (acquired_live_command && job_command_starts_work(cmd) &&
                    publication_sequence ==
                        jc->command_acquired_publication_sequence[command_slot]) {
                    /* Sequential traversal can beat a post-store callback
                     * just as a parked poll can. Record the acquisition before
                     * releasing the mutex so that callback cannot enqueue the
                     * command a second time. A notified same-value store has
                     * a newer publication sequence and remains new work. */
                    jc->command_polled_unacknowledged[command_slot] = cmd;
                }
                if (job_command_starts_work(cmd) && publication_sequence >
                    jc->command_acquired_publication_sequence[command_slot])
                    jc->command_acquired_publication_sequence[command_slot] =
                        publication_sequence;
                atomic_store_explicit(&jc->command_consumed[command_slot], 1,
                                      memory_order_release);
                if (is_job) {
                    jc->command_last_descriptor[command_slot] = (u32)cmd;
                    const u8* identity = acquired_descriptor;
                    u8 identity_copy[0x400];
                    if (!identity && !acquired_descriptor_fault &&
                        jc->descriptor_size <= sizeof(identity_copy) &&
                        job_descriptor_copy_stable(
                            (u32)cmd, jc->descriptor_size, identity_copy))
                        identity = identity_copy;
                    jc->command_last_identity_valid[command_slot] =
                        identity != NULL;
                    if (identity) {
                        jc->command_last_binary[command_slot] =
                            rd64(identity) & ~1ull;
                        jc->command_last_binary_blocks[command_slot] =
                            rd16(identity + 0x08);
                        jc->command_last_job_type[command_slot] =
                            jc->descriptor_size > 0x2cu ?
                                identity[0x2c] : 0;
                    }
                    jc->command_last_dispatch_sequence[command_slot] =
                        ++jc->command_dispatch_sequence_next;
                }
            }
        }
        jobchain_watch_ea_locked(jc, pc);
        if (pc <= 0xfffffff7u)
            jobchain_watch_ea_locked(jc, pc + 8u);
        if (jc->pool && acquired_descriptor) {
            memcpy(parallel_descriptor, acquired_descriptor, jc->descriptor_size);
            acquired_descriptor = parallel_descriptor;
        }
        mx_unlock(&jc->sync.mutex);
        /* Only ordinary adjacent jobs overlap. All control flow, labels,
         * guards, publication parks and bridge jobs drain the batch before
         * their original processing continues. */
        if (jc->pool && (!job_command_is_job(cmd) || acquired_bridge_publication)) {
            const int rc = job_pool_drain(jc->pool);
            if (rc) return rc;
        }
        u32 op = (u32)(cmd & 7), ext = (u32)(cmd & 127);
        if (cmd == 0x0000000800000012ull) {
            int rc = wait_job_slot(jc, pc, cmd);
            if (rc) return rc;
            pc = jobchain_get_current(jc);
            continue;
        }
        if (job_command_is_partial_publication(cmd)) {
            /* Narrow producer stores can leave the old empty-marker upper
             * word paired with any new command lower word while the write
             * callback is running.  Park on that exact mixed value until the
             * command generation is complete. */
            int rc = wait_job_slot(jc, pc, cmd);
            if (rc) return rc;
            pc = jobchain_get_current(jc);
            continue;
        }
        if (job_command_is_job(cmd)) {
            if (acquired_descriptor_fault)
                return CELL_SPURS_JOB_ERROR_FAULT;
            /* Pending-ticket bridge: keep every dispatched descriptor's
             * kind word (+0x10) inside the store-notify window so the
             * bridge in jobchain_notify_guest_write sees the submit. */
            mx_lock(&jc->sync.mutex);
            jobchain_bridge_watch_ea_locked(jc, (u32)cmd + 0x10);
            mx_unlock(&jc->sync.mutex);
            int rc = jc->pool && !acquired_bridge_publication ?
                job_pool_submit(jc->pool, (u32)cmd, jc->descriptor_size,
                                acquired_descriptor) :
                run_job(jc, (u32)cmd, jc->descriptor_size, acquired_descriptor);
            if (rc) return rc;
            if (acquired_bridge_publication)
                jobchain_note_bridge_success();
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
            JobListView joblist;
            int rc = joblist_acquire_published(jc, list, &joblist);
            if (rc) return rc;
            for (u32 i = 0; i < joblist.count; ++i) {
                const u32 descriptor_ea =
                    (u32)((u64)joblist.first_descriptor +
                          (u64)i * joblist.descriptor_size);
                rc = jc->pool ? job_pool_submit(jc->pool, descriptor_ea,
                             joblist.descriptor_size, NULL) :
                     run_job(jc, descriptor_ea, joblist.descriptor_size, NULL);
                if (rc) return rc;
            }
            pc += 8; jobchain_set_current(jc, pc); continue;
        }
        if (op == 7) {
            if (ext == 127) {                                  /* END */
                /* The command pointer stays on the END word, where a guest
                 * write still counts as a publication head
                 * (jobchain_plausible_publication_head_locked). The next
                 * cellSpursRunJobChain restarts from jobChainEntry. */
                jobchain_set_current(jc, pc);
                return CELL_OK;
            }
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
    /* Each Run restarts the scan from jobChainEntry rather than resuming at
     * the parked END. */
    jc->current_ea = jc->entry_ea;
    jc->call_depth = 0;
    jc->command_snapshot_count = 0;
    jc->command_snapshot_index = 0;
    jc->waiting_command = 0;
    jc->parked_slot_count = 0;
    jc->command_state_count = 0;
    memset(jc->command_state_ea, 0, sizeof(jc->command_state_ea));
    memset(jc->command_last_descriptor, 0,
           sizeof(jc->command_last_descriptor));
    memset(jc->command_last_binary, 0,
           sizeof(jc->command_last_binary));
    memset(jc->command_last_binary_blocks, 0,
           sizeof(jc->command_last_binary_blocks));
    memset(jc->command_last_job_type, 0,
           sizeof(jc->command_last_job_type));
    memset(jc->command_last_identity_valid, 0,
           sizeof(jc->command_last_identity_valid));
    memset(jc->command_last_dispatch_sequence, 0,
           sizeof(jc->command_last_dispatch_sequence));
    jc->command_dispatch_sequence_next = 0;
    memset(jc->parked_slot_ea, 0, sizeof(jc->parked_slot_ea));
    memset(jc->parked_slot_empty, 0, sizeof(jc->parked_slot_empty));
    for (u32 i = 0; i < MAX_JOB_PARKED_SLOTS; ++i)
        atomic_store_explicit(&jc->command_consumed[i], 0,
                              memory_order_relaxed);
    memset(jc->command_publication_sequence, 0,
           sizeof(jc->command_publication_sequence));
    memset(jc->command_acquired_publication_sequence, 0,
           sizeof(jc->command_acquired_publication_sequence));
    memset(jc->command_polled_unacknowledged, 0,
           sizeof(jc->command_polled_unacknowledged));
    memset(jc->deferred_resume_sequence, 0,
           sizeof(jc->deferred_resume_sequence));
    memset(jc->deferred_publication_sequence, 0,
           sizeof(jc->deferred_publication_sequence));
    jc->deferred_sequence_next = 0;
    jc->watch_lo = jc->entry_ea;
    jc->watch_hi = jc->entry_ea + 8u;
    jc->bridge_lo = UINT32_MAX;
    jc->bridge_hi = 0;
    jc->alternate_slot = 0x4c00;
    jc->next_dma_tag = 0;
    {
        const u32 index = (u32)(jc - g_jobchains);
        /* Retire exact command/descriptor watches from the previous run.
         * Append-only router entries remain harmless because their generation
         * no longer matches this target. */
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_JOBCHAIN_COMMAND, index);
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_JOBCHAIN_BRIDGE, index);
    }
    jobchain_watch_ea_locked(jc, jc->entry_ea);
}
static void jobchain_run(JobChainState* jc)
{
    jc->pool = job_pool_create(jc);
    int rc = execute_chain(jc);
    if (jc->pool) {
        const int completion_rc = job_pool_destroy(jc->pool);
        jc->pool = NULL;
        if (!rc) rc = completion_rc;
    }
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
    {
        const u32 index = (u32)(g - g_jobguards);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_JOBGUARD, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_JOBGUARD, index,
                                guest_ea(object), sizeof(u32));
    }
    return CELL_OK;
}
s32 cellSpursJobGuardNotify(CellSpursJobGuard* object)
{
    if (!object) return CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (!aligned(object, 128)) return CELL_SPURS_JOB_ERROR_ALIGN;
    JobGuardState* g = jobguard_find(object);
    if (!g) return CELL_SPURS_JOB_ERROR_STAT;
    mx_lock(&g->sync.mutex);
    /* The decrement must be an atomic on guest memory (lwarx/stwcx on
     * guard[0x00]), not a blind write-back of a host mirror: an SPU-side
     * putllc decrement landing between our read and store would be silently
     * overwritten and the guard never released. Decrement under the
     * lockline, re-derive the host mirror from the result. */
    /* job_publish_u32 takes the (non-reentrant) lockline itself, so
     * the store is done inline within one critical section. */
    {
        spu_lockline_lock();
        u32 live = rd32(object->bytes + 0x00);
        if (!live) {
            spu_lockline_unlock();
            mx_unlock(&g->sync.mutex);
            return CELL_SPURS_JOB_ERROR_STAT;
        }
        live--;
        wr32(object->bytes + 0x00, live);
        {
            const u32 line = guest_ea(object) & ~127u;
            if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        }
        spu_lockline_unlock();
        g->count = live;
    }
    if (!g->count) cv_wake_all(&g->sync.cond);
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
