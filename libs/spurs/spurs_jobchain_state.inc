/* Job chain state and publication tracking.
 * Part of cellSpurs.c, which includes its parts once, in order. */

/* ------------------------------------------------------------------------- */
/* Job chains and guards                                                     */

#define MAX_JOB_COMMAND_SNAPSHOT 512
/* Command state and parked-slot tables must cover the longest live command
 * list; when full, publication tracking degrades (see the SATURATED
 * diagnostics). */
#define MAX_JOB_PARKED_SLOTS 4096
typedef struct {
    u32 ea;
    u64 command;
    u64 publication_sequence;
    u16 descriptor_size;
    u8 descriptor_status; /* 0=not a job, 1=stable snapshot, 2=fault */
    u8 bridge_publication;
    u8 descriptor[0x400];
} JobCommandSnapshot;

typedef struct PendingJobSnapshot {
    struct PendingJobSnapshot* next;
    u32 start_ea;
    u32 count;
    u64 resume_sequence; /* explicit deferred JOB publication; zero for a barrier/bridge */
    JobCommandSnapshot command[];
} PendingJobSnapshot;

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
    u32 watch_lo, watch_hi;
    /* Pending-ticket bridge window: dispatched descriptors' kind words.
     * Kept separate from watch_lo/hi: routing ring stores through the
     * command machinery would saturate the parked/command tables and
     * degrade publication tracking. */
    u32 bridge_lo, bridge_hi;
    u32 command_state_ea[MAX_JOB_PARKED_SLOTS];
    /* The command ring is mutable: after a JOB has been acquired, the PPU may
     * recycle its slot before publishing the next ticket for the persistent
     * dispatcher.  Keep the acquired slot -> descriptor relationship instead
     * of trying to reconstruct it from the recycled guest word. */
    u32 command_last_descriptor[MAX_JOB_PARKED_SLOTS];
    /* A descriptor address is no more durable than its command-ring slot.
     * Retained mappings therefore also remember the immutable binary identity
     * acquired with the command.  This prevents a recycled descriptor arena
     * containing ordinary data from satisfying the very broad kind-6 ticket
     * predicate and being redispatched as a job. */
    u64 command_last_binary[MAX_JOB_PARKED_SLOTS];
    u16 command_last_binary_blocks[MAX_JOB_PARKED_SLOTS];
    u8 command_last_job_type[MAX_JOB_PARKED_SLOTS];
    u8 command_last_identity_valid[MAX_JOB_PARKED_SLOTS];
    u64 command_last_dispatch_sequence[MAX_JOB_PARKED_SLOTS];
    u64 command_dispatch_sequence_next;
    u32 command_state_count;
    u32 parked_slot_ea[MAX_JOB_PARKED_SLOTS];
    u64 parked_slot_empty[MAX_JOB_PARKED_SLOTS];
    u32 parked_slot_count;
    _Atomic u8 command_consumed[MAX_JOB_PARKED_SLOTS];
    u64 command_publication_sequence[MAX_JOB_PARKED_SLOTS];
    u64 command_acquired_publication_sequence[MAX_JOB_PARKED_SLOTS];
    u64 command_polled_unacknowledged[MAX_JOB_PARKED_SLOTS];
    u64 deferred_resume_sequence[MAX_JOB_PARKED_SLOTS];
    u64 deferred_publication_sequence[MAX_JOB_PARKED_SLOTS];
    u64 deferred_sequence_next;
    int waiting_command;
    u32 alternate_slot;
    u32 dma_tag[2];
    u32 next_dma_tag;
    nthread thread;
    int thread_valid, joined;
    u32 max_contention, parallel_capacity;
    u32 parallel_spu_ids[2];
    struct JobPool* pool;
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

/* A lifted PPU store callback runs after the individual store, not after the
 * guest's publication barrier. Keep candidate descriptor addresses local to
 * that producer thread and release them only when its next lifted sync/hwsync
 * executes. This is the host-side acquire edge that a free-running SPURS
 * kernel gets from the PPU's memory-ordering protocol. */
#define MAX_PPU_BRIDGE_CANDIDATES 16
#if defined(_WIN32)
static __declspec(thread) u32 g_ppu_bridge_candidate[MAX_PPU_BRIDGE_CANDIDATES];
static __declspec(thread) u32 g_ppu_bridge_candidate_count;
#else
static _Thread_local u32 g_ppu_bridge_candidate[MAX_PPU_BRIDGE_CANDIDATES];
static _Thread_local u32 g_ppu_bridge_candidate_count;
#endif

static void jobchain_stage_ppu_bridge_candidate(u32 descriptor)
{
    for (u32 i = 0; i < g_ppu_bridge_candidate_count; ++i)
        if (g_ppu_bridge_candidate[i] == descriptor)
            return;
    if (g_ppu_bridge_candidate_count < MAX_PPU_BRIDGE_CANDIDATES)
        g_ppu_bridge_candidate[g_ppu_bridge_candidate_count++] = descriptor;
}

static void jobchain_watch_ea_locked(JobChainState* jc, u32 ea)
{
    if (!ea) return;
    guest_write_route_watch(
        GUEST_WRITE_ROUTE_JOBCHAIN_COMMAND,
        (u32)(jc - g_jobchains), ea, sizeof(u64));
    if (ea < jc->watch_lo) jc->watch_lo = ea;
    if (ea <= 0xfffffff7u && ea + 8u > jc->watch_hi)
        jc->watch_hi = ea + 8u;
    if (jc->watch_lo < g_native_spurs_ppu_watch_lo)
        g_native_spurs_ppu_watch_lo = jc->watch_lo;
    if (jc->watch_hi > g_native_spurs_ppu_watch_hi)
        g_native_spurs_ppu_watch_hi = jc->watch_hi;
}

static void jobchain_bridge_watch_ea_locked(JobChainState* jc, u32 ea)
{
    if (!ea) return;
    guest_write_route_watch(
        GUEST_WRITE_ROUTE_JOBCHAIN_BRIDGE,
        (u32)(jc - g_jobchains), ea, sizeof(u64));
    if (ea < jc->bridge_lo) jc->bridge_lo = ea;
    if (ea <= 0xfffffff7u && ea + 8u > jc->bridge_hi)
        jc->bridge_hi = ea + 8u;
    /* The global store-notify window must span both windows. */
    if (jc->bridge_lo < g_native_spurs_ppu_watch_lo)
        g_native_spurs_ppu_watch_lo = jc->bridge_lo;
    if (jc->bridge_hi > g_native_spurs_ppu_watch_hi)
        g_native_spurs_ppu_watch_hi = jc->bridge_hi;
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

static int job_command_is_job(u64 command)
{
    /* CellSpursJobChain commands carry a 32-bit main-storage EA.  The upper
     * word is reserved and zero for JOB.  Rejecting a nonzero upper word is
     * also the publication-completeness check for producers that replace the
     * 0x00000008_00000012 empty marker with narrower stores: the intermediate
     * mixed generation must not be executed as a descriptor pointer. */
    return command && (command >> 32) == 0u && (command & 7u) == 0u;
}

static int job_command_is_partial_publication(u64 command)
{
    /* Every SDK job-chain command is a 32-bit main-storage EA/opcode in the
     * low word; the upper word is reserved and zero.  The title replaces its
     * 0x00000008_00000012 empty marker with narrower stores, so every opcode
     * (not only JOB) can briefly be paired with the marker's stale upper
     * word.  The marker itself is a stable empty predicate, not a torn
     * command. */
    return command != 0x0000000800000012ull && (command >> 32) != 0u;
}

static int job_command_starts_work(u64 command)
{
    const u32 op = (u32)(command & 7u);
    return job_command_is_job(command) || op == 6u;
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

static int job_descriptor_snapshot_publishable(const u8* descriptor,
                                                u16 size)
{
    if (!descriptor || size < 0x30u)
        return 0;
    const u64 binary = rd64(descriptor + 0x00) & ~1ull;
    const u32 binary_size = (u32)rd16(descriptor + 0x08) * 16u;
    return binary != 0u && binary_size != 0u &&
           binary <= 0xffffffffull &&
           job_guest_range_valid((u32)binary, binary_size);
}

static int job_descriptor_acquire_published(u32 ea, u16 size,
                                            u8* destination)
{
    for (u32 attempt = 0; attempt < 64u; ++attempt) {
        if (job_descriptor_copy_stable(ea, size, destination) &&
            job_descriptor_snapshot_publishable(destination, size))
            return 1;
        if (attempt < 8u)
            nthread_yield();
        else
            nthread_reschedule();
    }
    return 0;
}

typedef struct {
    u32 count;
    u32 descriptor_size;
    u32 first_descriptor;
} JobListView;

/* A JOBLIST command can become visible to the native worker while the lifted
 * PPU producer is still publishing the 16-byte CellSpursJobList it names.
 * Real SPURS observes the producer's synchronization ordering; scheduling the
 * native worker from a per-store callback can instead expose an intermediate
 * {numJobs, sizeOfJob, eaJobList} generation.  Acquire two identical copies
 * and give an incomplete/invalid generation a short publication grace before
 * reporting the SDK error.  This is deliberately local to the 16-byte object:
 * widening the chain's coarse command watch window down to a low-EA JobList
 * would route most title stores through the job-chain mutex. */
static int joblist_acquire_published(JobChainState* jc, u32 ea,
                                     JobListView* view)
{
    u8 bytes[16];
    u8 verify[16];
    int last_error = CELL_SPURS_JOB_ERROR_FAULT;
    for (u32 attempt = 0; attempt < 64u; ++attempt) {
        memcpy(bytes, vm_base + ea, sizeof(bytes));
        atomic_thread_fence(memory_order_seq_cst);
        memcpy(verify, vm_base + ea, sizeof(verify));
        atomic_thread_fence(memory_order_acquire);
        if (!memcmp(bytes, verify, sizeof(bytes))) {
            const u32 count = rd32(bytes + 0x00);
            const u32 descriptor_size = rd32(bytes + 0x04);
            const u64 first = rd64(bytes + 0x08);
            if (descriptor_size < 0x30u ||
                (descriptor_size & 0x0fu)) {
                last_error = CELL_SPURS_JOB_ERROR_DESCRIPTOR;
            } else {
                const u64 list_bytes = (u64)count * descriptor_size;
                if ((first >> 32) != 0u ||
                    (count && !job_guest_range_valid((u32)first,
                                                     list_bytes))) {
                    last_error = CELL_SPURS_JOB_ERROR_FAULT;
                } else {
                    view->count = count;
                    view->descriptor_size = descriptor_size;
                    view->first_descriptor = (u32)first;
                    return CELL_OK;
                }
            }
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
            extern void yz_spurs_joblist_publication_test_hook(u32, u32);
            yz_spurs_joblist_publication_test_hook(ea, attempt);
#endif
        }
        if (atomic_load(&jc->shutdown))
            return CELL_SPURS_JOB_ERROR_ABORT;
        if (attempt < 8u)
            nthread_yield();
        else
            nthread_reschedule();
    }
    memcpy(bytes, vm_base + ea, sizeof(bytes));
    fprintf(stderr,
            "[jc-joblist] invalid after publication grace list=%08X "
            "words=%08X/%08X/%08X/%08X error=0x%08X\n",
            ea, rd32(bytes + 0x00), rd32(bytes + 0x04),
            rd32(bytes + 0x08), rd32(bytes + 0x0c),
            (u32)last_error);
    fflush(stderr);
    return last_error;
}

static int jobchain_command_slot(JobChainState* jc, u32 ea, int create)
{
    if (!ea || (ea & 7u)) return -1;
    for (u32 i = 0; i < jc->command_state_count; ++i)
        if (jc->command_state_ea[i] == ea) return (int)i;
    if (!create || jc->command_state_count == MAX_JOB_PARKED_SLOTS) {
        if (create) {
            static atomic_uint sat_n;
            const unsigned k = atomic_fetch_add(&sat_n, 1u);
            if (k < 8u)
                fprintf(stderr, "[jc-SATURATED] command_state full (%u) "
                        "ea=%08X; publication tracking degraded\n",
                        (unsigned)MAX_JOB_PARKED_SLOTS, ea);
        }
        return -1;
    }
    const u32 slot = jc->command_state_count++;
    jc->command_state_ea[slot] = ea;
    return (int)slot;
}

static int jobchain_plausible_publication_head_locked(
    const JobChainState* jc, u32 ea)
{
    if (ea == jc->entry_ea || ea == jc->current_ea)
        return 1;
    /* A producer publishes the head of a sequential command prefix last.
     * Before the worker reaches that head, its predecessor is the strongest
     * exact evidence available.  Keep this sparse: watch_lo/watch_hi is only
     * a cheap callback prefilter and may span megabytes when CALL jumps from
     * a main-storage ring into a distant sub-list. */
    for (u32 i = 0; i < jc->command_state_count; ++i) {
        const u32 known = jc->command_state_ea[i];
        if (known == ea || (known <= 0xfffffff7u && known + 8u == ea))
            return 1;
    }
    for (u32 i = 0; i < jc->parked_slot_count; ++i) {
        const u32 known = jc->parked_slot_ea[i];
        if (known == ea || (known <= 0xfffffff7u && known + 8u == ea))
            return 1;
    }
    for (u32 i = 0; i < jc->command_snapshot_count; ++i) {
        const u32 known = jc->command_snapshot[i].ea;
        if (known == ea || (known <= 0xfffffff7u && known + 8u == ea))
            return 1;
    }
    return 0;
}

/*
 * A command-ring producer fills the body first and publishes its head last.
 * Claim the resulting immutable command view before returning from the guest
 * write notification.  The producer may reuse consumed slots while jobs run;
 * parsing directly from mutable guest memory after each synchronous host job
 * would therefore observe a mixture of two publications.
 */

static u32 jobchain_capture_commands(JobChainState* jc, u32 start_ea,
                                     JobCommandSnapshot* snapshot,
                                     int grab_bounded)
{
    u32 pc = start_ea;
    u32 stack[16];
    u32 depth = jc->call_depth;
    if (depth > 16) depth = 16;
    memcpy(stack, jc->call_stack, depth * sizeof(stack[0]));

    u32 count = 0;
    /* Bound the stable-snapshot "grab window" like hardware. The SPU
     * fetches the command list in 128-byte units and grabs at most
     * maxGrabbedJob jobs per grab; walking up to 512 commands across the
     * whole chain would widen the stale-view window ~100x into the
     * producer's legally-mutating region. Sequential advancement past the
     * current 128-byte line ends the snapshot (the executor falls back to
     * live reads / a fresh capture there); flow control (NEXT/CALL/RET)
     * models a new fetch and re-bases the line.
     * The job budget spans the whole capture.
     * grab_bounded=0 is the publication-time snapshot (the producer's
     * published prefix, bounded by the barrier set, not by the grab
     * window -- a published stable view must survive producer slot reuse
     * regardless of maxGrabbedJob). grab_bounded=1 is the claim-time fetch. */
    const u32 grab_cap = jc->max_grab ? jc->max_grab : 16u;
    u32 jobs_grabbed = 0;
    u32 fetch_line = start_ea & ~127u;
    while (count < MAX_JOB_COMMAND_SNAPSHOT) {
        if (grab_bounded && (pc & ~127u) != fetch_line)
            break;                        /* 128-byte boundary stop */
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
        if (!command || job_command_is_empty(pc, command))
            break;
        /* Never immortalize an unpublished zero or a mixed upper/lower word
         * in an immutable generation.  This is essential for CALL: a
         * snapshot that follows the published CALL into a dynamic sub-list
         * before its tail RET appears would preserve that zero and then walk
         * through unrelated guest data until it decoded an accidental END.
         * The completing store must instead remain visible to the live
         * executor. */
        if (job_command_is_partial_publication(command))
            break;
        if (grab_bounded && job_command_is_job(command)) {
            if (jobs_grabbed == grab_cap)
                break;                    /* maxGrabbedJob bound */
            ++jobs_grabbed;
        }

        snapshot[count].ea = pc;
        snapshot[count].command = command;
        snapshot[count].descriptor_size = 0;
        snapshot[count].descriptor_status = 0;
        snapshot[count].bridge_publication = 0;
        {
            const int slot = jobchain_command_slot(jc, pc, 0);
            snapshot[count].publication_sequence = slot >= 0 ?
                jc->command_publication_sequence[slot] : 0;
        }
        if (job_command_is_job(command) &&
            jc->descriptor_size <= 0x400u) {
            const u32 descriptor_ea = (u32)command;
            snapshot[count].descriptor_status = 2;
            if (job_descriptor_copy_stable(descriptor_ea,
                                           jc->descriptor_size,
                                           snapshot[count].descriptor) &&
                job_descriptor_snapshot_publishable(
                    snapshot[count].descriptor, jc->descriptor_size)) {
                snapshot[count].descriptor_size = jc->descriptor_size;
                snapshot[count].descriptor_status = 1;
            } else {
                /* A stable all-zero arena is not a published descriptor.
                 * Stop the immutable claim before this JOB so a later live
                 * fetch can acquire the generation after the producer has
                 * completed it.  Capturing an old zero copy would fail
                 * INVALID_BIN even though the live descriptor is complete by
                 * the time execution arrives. */
                break;
            }
        }
        ++count;

        if (job_command_is_job(command) || op == 2u || op == 5u || op == 6u) {
            pc += 8;
            continue;
        }
        if (op == 1u || op == 3u) {
            if (op == 1u) depth = 0;
            pc = (u32)(command & ~7ull);
            fetch_line = pc & ~127u;      /* new fetch */
            continue;
        }
        if (op == 4u) {
            if (depth == 16) break;
            stack[depth++] = pc + 8;
            pc = (u32)(command & ~7ull);
            fetch_line = pc & ~127u;      /* new fetch */
            continue;
        }
        if (op == 7u) {
            if (ext == 119u) {
                if (!depth) break;
                pc = stack[--depth];
                fetch_line = pc & ~127u;  /* new fetch */
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
    /* grab_bounded=0: this claim consumes a published view (parked-slot
     * resume). Bounding it by maxGrabbedJob breaks the publication-claim
     * protocol -- the producer legally reuses consumed slots immediately
     * after notify, trusting the claim took the whole published prefix.
     * A bound here must clip by published extent instead. */
    const u32 count = jobchain_capture_commands(
        jc, start_ea, jc->command_snapshot, /*grab_bounded=*/0);
    jc->command_snapshot_count = count;
    jc->command_snapshot_index = 0;
    return count;
}

static u32 jobchain_claim_polled(JobChainState* jc, u32 start_ea)
{
    const u32 count = jobchain_claim_current(jc, start_ea);
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
    if (!count) {
        extern void yz_spurs_failed_poll_test_hook(u32);
        yz_spurs_failed_poll_test_hook(start_ea);
    }
#endif
    if (count) {
        const int slot = jobchain_command_slot(jc, start_ea, 1);
        if (slot >= 0)
            jc->command_polled_unacknowledged[slot] =
                jc->command_snapshot[0].command;
    }
    return count;
}

static int jobchain_ack_polled_command(JobChainState* jc, u32 ea, u64 command)
{
    const int slot = jobchain_command_slot(jc, ea, 0);
    if (slot < 0) return 0;
    const u64 polled = jc->command_polled_unacknowledged[slot];
    jc->command_polled_unacknowledged[slot] = 0;
    /* A compatibility poll can acquire the parked head between the producer's
     * store and its post-store callback. That first matching callback owns no
     * new work. Retire the acknowledgement immediately: a subsequent same-
     * value publication is still a new generation. An observed clear or a
     * different command also retires it, allowing circular slot reuse. */
    return polled && polled == command;
}

static u32 jobchain_claim_persistent_job(JobChainState* jc, u32 slot_ea,
                                         u32 descriptor_ea,
                                         const u8* acquired_descriptor)
{
    const u64 live_command = rd64(vm_base + slot_ea);
    if (job_command_is_job(live_command) &&
        (u32)live_command == descriptor_ea) {
        const u32 count = jobchain_claim_current(jc, slot_ea);
        if (count && jc->command_snapshot[0].ea == slot_ea &&
            job_command_is_job(jc->command_snapshot[0].command) &&
            (u32)jc->command_snapshot[0].command == descriptor_ea) {
            /* Selection below already acquired a stable descriptor while this
             * exact command generation was live.  Do not replace it with a
             * later descriptor-arena generation captured while walking the
             * rest of the newly published command prefix. */
            JobCommandSnapshot* snapshot = &jc->command_snapshot[0];
            snapshot->descriptor_size = jc->descriptor_size;
            snapshot->descriptor_status = 1;
            snapshot->bridge_publication = 1;
            memcpy(snapshot->descriptor, acquired_descriptor,
                   jc->descriptor_size);
            return count;
        }
    }

    /* The SPURS kernel has already acquired this persistent JOB command.  A
     * producer is therefore free to recycle the main-memory ring slot while
     * the dispatcher continues to orbit it.  Recreate that one acquired
     * command from command state, including a stable descriptor snapshot, so
     * redispatch does not depend on the current contents of the ring slot. */
    JobCommandSnapshot* snapshot = &jc->command_snapshot[0];
    snapshot->ea = slot_ea;
    snapshot->command = descriptor_ea;
    snapshot->publication_sequence = 0;
    snapshot->descriptor_size = jc->descriptor_size;
    snapshot->descriptor_status = 1;
    snapshot->bridge_publication = 1;
    memcpy(snapshot->descriptor, acquired_descriptor, jc->descriptor_size);
    jc->command_snapshot_count = 1;
    jc->command_snapshot_index = 0;
    return 1;
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
    } else {
        static atomic_uint psat_n;
        const unsigned k = atomic_fetch_add(&psat_n, 1u);
        if (k < 8u)
            fprintf(stderr, "[jc-SATURATED] parked_slot full (%u) "
                    "ea=%08X; wrapped publications unwatched\n",
                    (unsigned)MAX_JOB_PARKED_SLOTS, ea);
    }
}

static u64 jobchain_defer_resume(JobChainState* jc, u32 ea,
                                  u64 publication_sequence)
{
    const int slot = jobchain_command_slot(jc, ea, 1);
    if (slot < 0) return 0;
    const u64 sequence = ++jc->deferred_sequence_next;
    if (jc->deferred_resume_sequence[slot]) {
        if (publication_sequence > jc->deferred_publication_sequence[slot])
            jc->deferred_publication_sequence[slot] = publication_sequence;
        return sequence;
    }
    jc->deferred_resume_sequence[slot] = sequence;
    jc->deferred_publication_sequence[slot] = publication_sequence;
    return sequence;
}

static void jobchain_cancel_deferred_resume(JobChainState* jc, u32 ea,
                                            u64 publication_sequence)
{
    const int slot = jobchain_command_slot(jc, ea, 0);
    if (slot < 0 || !jc->deferred_resume_sequence[slot] ||
        jc->deferred_publication_sequence[slot] > publication_sequence)
        return;
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


static void jobchain_supersede_pending_callee(JobChainState* jc, u32 target)
{
    /* An incoming CALL publishes a rebuilt sub-list. Stores of barriers
     * inside that list can precede its new tail. Their speculative queued views must not override this later
     * publication. Keep already acquired command_snapshot bytes intact.
     * Do not retire a view belonging to a call that is still executing. */
    if (!jc->pending_snapshot_head || jc->call_depth)
        return;
    u32 pc = target, stack[16], depth = 0;
    for (u32 n = 0; n < MAX_JOB_COMMAND_SNAPSHOT; ++n) {
        if (!job_guest_range_valid(pc, sizeof(u64))) break;
        const u64 command = rd64(vm_base + pc);
        if (!command || job_command_is_empty(pc, command) ||
            job_command_is_partial_publication(command)) break;
        PendingJobSnapshot* previous = NULL;
        PendingJobSnapshot* pending = jc->pending_snapshot_head;
        while (pending) {
            PendingJobSnapshot* next = pending->next;
            if (pending->start_ea == pc) {
                if (previous) previous->next = next;
                else jc->pending_snapshot_head = next;
                if (jc->pending_snapshot_tail == pending)
                    jc->pending_snapshot_tail = previous;
                free(pending);
            } else previous = pending;
            pending = next;
        }
        if (!jc->pending_snapshot_head) break;
        const u32 op = (u32)(command & 7u), ext = (u32)(command & 127u);
        if (job_command_is_job(command) || op == 2u || op == 5u || op == 6u)
            pc += 8u;
        else if (op == 3u)
            pc = (u32)(command & ~7ull);
        else if (op == 4u) {
            if (depth == 16u) break;
            stack[depth++] = pc + 8u;
            pc = (u32)(command & ~7ull);
        } else if (op == 7u && ext == 119u) {
            if (!depth) break;
            pc = stack[--depth];
        } else if (op == 7u && (ext == 15u || ext == 23u))
            pc += 8u;
        else break; /* END, RESET_PC or unsupported flow leaves this callee. */
    }
}

static u32 jobchain_queue_snapshot(JobChainState* jc, u32 start_ea,
                                   int replace_unconsumed)
{
    PendingJobSnapshot* snapshot =
        (PendingJobSnapshot*)malloc(sizeof(*snapshot) +
            MAX_JOB_COMMAND_SNAPSHOT * sizeof(snapshot->command[0]));
    if (!snapshot) {
        return 0;
    }
    snapshot->next = NULL;
    snapshot->resume_sequence = 0;
    snapshot->start_ea = start_ea;
    snapshot->count = jobchain_capture_commands(
        jc, start_ea, snapshot->command, /*grab_bounded=*/0);
    if (!snapshot->count) {
        free(snapshot);
        return 0;
    }
    if (replace_unconsumed) {
        PendingJobSnapshot* previous = NULL;
        PendingJobSnapshot* queued = jc->pending_snapshot_head;
        while (queued && queued->start_ea != start_ea) {
            previous = queued;
            queued = queued->next;
        }
        if (queued) {
            snapshot->next = queued->next;
            if (previous)
                previous->next = snapshot;
            else
                jc->pending_snapshot_head = snapshot;
            if (jc->pending_snapshot_tail == queued)
                jc->pending_snapshot_tail = snapshot;
            free(queued);
            return snapshot->count;
        }
    }
    if (jc->pending_snapshot_tail)
        jc->pending_snapshot_tail->next = snapshot;
    else
        jc->pending_snapshot_head = snapshot;
    jc->pending_snapshot_tail = snapshot;
    return snapshot->count;
}

/* A circular slot may have several published JOB generations waiting while
 * the worker runs its prefix. Keep the next one's original arrival order,
 * rather than letting consumption of an older generation erase its wake. */
static void jobchain_refresh_queued_job_resume(JobChainState* jc, u32 ea)
{
    for (PendingJobSnapshot* p = jc->pending_snapshot_head; p; p = p->next) {
        if (p->start_ea != ea || !p->resume_sequence) continue;
        const int slot = jobchain_command_slot(jc, ea, 1);
        if (slot >= 0) {
            jc->deferred_resume_sequence[slot] = p->resume_sequence;
            if (p->command[0].publication_sequence > jc->deferred_publication_sequence[slot])
                jc->deferred_publication_sequence[slot] = p->command[0].publication_sequence;
        }
        break;
    }
}

static void jobchain_discard_consumed_snapshot(JobChainState* jc,
                                               u32 start_ea,
                                               u64 publication_sequence)
{
    if (!publication_sequence) return;
    PendingJobSnapshot* previous = NULL;
    PendingJobSnapshot* snapshot = jc->pending_snapshot_head;
    while (snapshot) {
        PendingJobSnapshot* next = snapshot->next;
        const u64 queued_sequence = snapshot->count ?
            snapshot->command[0].publication_sequence : 0;
        if (snapshot->start_ea == start_ea && queued_sequence &&
            queued_sequence <= publication_sequence) {
            if (previous)
                previous->next = next;
            else
                jc->pending_snapshot_head = next;
            if (jc->pending_snapshot_tail == snapshot)
                jc->pending_snapshot_tail = previous;
            free(snapshot);
        } else {
            previous = snapshot;
        }
        snapshot = next;
    }
    jobchain_refresh_queued_job_resume(jc, start_ea);
}

static u32 jobchain_queue_published_job(JobChainState* jc, u32 ea,
                                       u32 descriptor_ea, u64 publication,
                                       u64 resume_sequence)
{
    if (!resume_sequence || jc->descriptor_size > 0x400u) return 0;
    /* Retain just this published JOB, not speculative successors or an
     * entire 512-command allocation for every streamed descriptor. */
    PendingJobSnapshot* snapshot = (PendingJobSnapshot*)calloc(
        1, sizeof(*snapshot) + sizeof(snapshot->command[0]));
    if (!snapshot) {
        return 0;
    }
    JobCommandSnapshot* command = &snapshot->command[0];
    if (!job_descriptor_copy_stable(descriptor_ea, jc->descriptor_size, command->descriptor) ||
        !job_descriptor_snapshot_publishable(command->descriptor, jc->descriptor_size)) {
        free(snapshot);
        return 0; /* An incomplete descriptor still requires a later live fetch. */
    }
    snapshot->start_ea = ea;
    snapshot->count = 1;
    snapshot->resume_sequence = resume_sequence;
    command->ea = ea;
    command->command = descriptor_ea;
    command->publication_sequence = publication;
    command->descriptor_size = jc->descriptor_size;
    command->descriptor_status = 1;
    if (jc->pending_snapshot_tail) jc->pending_snapshot_tail->next = snapshot;
    else jc->pending_snapshot_head = snapshot;
    jc->pending_snapshot_tail = snapshot;
    return 1;
}

static u32 jobchain_queue_persistent_job(
    JobChainState* jc, u32 slot_ea, u32 descriptor_ea,
    const u8* acquired_descriptor)
{
    PendingJobSnapshot* snapshot =
        (PendingJobSnapshot*)calloc(1, sizeof(*snapshot) + sizeof(snapshot->command[0]));
    if (!snapshot) {
        return 0;
    }
    snapshot->start_ea = slot_ea;
    snapshot->count = 1;
    snapshot->command[0].ea = slot_ea;
    snapshot->command[0].command = descriptor_ea;
    snapshot->command[0].descriptor_size = jc->descriptor_size;
    snapshot->command[0].descriptor_status = 1;
    snapshot->command[0].bridge_publication = 1;
    memcpy(snapshot->command[0].descriptor, acquired_descriptor,
           jc->descriptor_size);
    if (jc->pending_snapshot_tail)
        jc->pending_snapshot_tail->next = snapshot;
    else
        jc->pending_snapshot_head = snapshot;
    jc->pending_snapshot_tail = snapshot;
    return 1;
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
    jobchain_refresh_queued_job_resume(jc, start_ea);
    memcpy(jc->command_snapshot, snapshot->command,
           snapshot->count * sizeof(jc->command_snapshot[0]));
    jc->command_snapshot_count = snapshot->count;
    jc->command_snapshot_index = 0;
    const u32 count = snapshot->count;
    free(snapshot);
    return count;
}

/* The title's synchronous producer exposes descriptor+0x10 = 6 and a nonzero
 * size_io before it finishes the descriptor body, executes hwsync, and waits
 * for the persistent dispatcher JOB to clear the ticket. A raw store edge is
 * therefore only a candidate. Lifted PPU writes become eligible at the same
 * producer thread's next guest fence; completed SPU DMA/HLE transfers carry
 * their own publication edge. This avoids a timing-based poll of partially
 * constructed guest memory. */
static u32 jobchain_resume_pending_ticket_locked(JobChainState* jc,
                                                 u32 descriptor_filter)
{
    int selected = 0;
    u32 selected_slot_ea = 0;
    u32 selected_desc = 0;
    u64 selected_sequence = 0;
    u8 selected_descriptor[0x400];
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
    /* Make accidental use before acquisition deterministic in the native
     * bridge-race regression instead of depending on host stack contents. */
    memset(selected_descriptor, 0xa5, sizeof(selected_descriptor));
#endif
    for (u32 m = 0; m < jc->command_state_count; ++m) {
        const u32 slot_ea = jc->command_state_ea[m];
        if (!job_guest_range_valid(slot_ea, 8)) continue;
        const u64 command = rd64(vm_base + slot_ea);
        const u32 live_desc = job_command_is_job(command) ?
            (u32)command : 0u;
        if (!live_desc && command && (command & 7u) == 0u &&
            (command >> 32) != 0u)
            continue;
        const u32 desc = live_desc ? live_desc :
            jc->command_last_descriptor[m];
        if (!desc) continue;
        if (descriptor_filter && desc != descriptor_filter) continue;
        /* A retained mapping represents the persistent dispatcher command the
         * kernel already acquired. The raw PPU store callback only stages a
         * candidate; reaching here means the producer's barrier has published
         * the complete descriptor. If the dispatcher is still running, queue
         * this immutable generation for its next orbit instead of re-reading
         * the recyclable command or descriptor arenas later. */
        if (desc > 0xffffffebu ||
            !job_guest_range_valid(desc + 0x10u, 8u))
            continue;
        u8 candidate_descriptor[0x400];
        if (jc->descriptor_size > sizeof(candidate_descriptor) ||
            !job_descriptor_copy_stable(
                desc, jc->descriptor_size, candidate_descriptor) ||
            !job_descriptor_snapshot_publishable(
                candidate_descriptor, jc->descriptor_size))
            continue;
        if (live_desc) {
            /* Acquire the descriptor and command as one generation.  The
             * producer may recycle either arena as soon as this persistent
             * command has been observed, so the later claim must never depend
             * on re-reading mutable guest memory. */
            atomic_thread_fence(memory_order_seq_cst);
            if (rd64(vm_base + slot_ea) != command)
                continue;
        } else {
            /* The command slot and descriptor arena are independently
             * recyclable.  A retained slot->descriptor address is usable
             * only while the descriptor still names the exact binary that
             * was acquired with that command generation. */
            if (!jc->command_last_identity_valid[m] ||
                (rd64(candidate_descriptor) & ~1ull) !=
                    jc->command_last_binary[m] ||
                rd16(candidate_descriptor + 0x08) !=
                    jc->command_last_binary_blocks[m] ||
                (jc->descriptor_size > 0x2cu &&
                 candidate_descriptor[0x2c] !=
                    jc->command_last_job_type[m])) {
                jc->command_last_descriptor[m] = 0;
                jc->command_last_dispatch_sequence[m] = 0;
                jc->command_last_identity_valid[m] = 0;
                continue;
            }
        }
        const u8* ticket_descriptor = candidate_descriptor;
        const u32 ticket = rd32(ticket_descriptor + 0x10u);
        const u32 ready = rd32(ticket_descriptor + 0x14u);
        if (ticket != 6u || ready == 0u)
            continue;
        /* A long-lived circular list can acquire the same persistent
         * descriptor through several recycled command slots.  The most recent
         * acquisition is the active kernel orbit; an older historical slot
         * carries stale continuation context and must not be redispatched. */
        const u64 sequence = jc->command_last_dispatch_sequence[m];
        if (!selected || sequence > selected_sequence) {
            selected = 1;
            selected_slot_ea = slot_ea;
            selected_desc = desc;
            selected_sequence = sequence;
            memcpy(selected_descriptor, candidate_descriptor,
                   jc->descriptor_size);
        }
    }
    if (!selected)
        return 0;

#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
    {
        extern void yz_spurs_pending_ticket_claim_test_hook(u32, u32);
        yz_spurs_pending_ticket_claim_test_hook(
            selected_slot_ea, selected_desc);
    }
#endif
    if (jc->waiting_command) {
        jc->current_ea = selected_slot_ea;
        if (jobchain_claim_persistent_job(
                jc, selected_slot_ea, selected_desc,
                selected_descriptor))
            jc->waiting_command = 0;
    } else {
        const int cs = jobchain_command_slot(jc, selected_slot_ea, 1);
        if (jobchain_queue_persistent_job(
                jc, selected_slot_ea, selected_desc,
                selected_descriptor))
            jobchain_defer_resume(jc, selected_slot_ea,
                                  cs >= 0 ?
                                  ++jc->command_publication_sequence[cs] : 0);
    }
    return 1;
}

static int jobchain_pending_ticket_publish_candidate(
    JobChainState* jc, u32 ea, u32 size, int exact_watched,
    u32* descriptor_out)
{
    const u64 first = ea;
    const u64 last = first + size;
    /* CellSpursJob descriptors are 16-byte aligned, not necessarily
     * 128-byte aligned. The readiness pair lives in the following aligned
     * 16-byte lane. A 128-byte mask would attribute the +0x14 store of a
     * descriptor such as 0x40603BB0 to unrelated storage. */
    const u32 header_lane = size ? (u32)(last - 1u) & ~0x0fu : 0u;
    const u32 descriptor = header_lane >= 0x10u ?
        header_lane - 0x10u : 0u;
    const u32 descriptor_offset = ea - descriptor;
    const int candidate =
        exact_watched && size <= 8u &&
        descriptor_offset < 0x18u &&
        descriptor_offset + size > 0x10u &&
        job_guest_range_valid(descriptor + 0x10u, 8u) &&
        rd32(vm_base + descriptor + 0x10u) == 6u &&
        rd32(vm_base + descriptor + 0x14u) != 0u;
    if (candidate && descriptor_out)
        *descriptor_out = descriptor;
    return candidate;
}

#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
int yz_spurs_test_watch_pending_descriptor(
    CellSpursJobChain* object, u32 descriptor)
{
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        JobChainState* jc = &g_jobchains[i];
        if (jc->sync.live && jc->sync.key == object) {
            jobchain_bridge_watch_ea_locked(jc, descriptor + 0x10u);
            return 0;
        }
    }
    return -1;
}

int yz_spurs_test_pending_ticket_publish_candidate(
    CellSpursJobChain* object, u32 ea, u32 size, u32* descriptor_out)
{
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        JobChainState* jc = &g_jobchains[i];
        if (jc->sync.live && jc->sync.key == object) {
            GuestWriteTargets targets;
            guest_write_route_lookup(ea, size, &targets);
            return jobchain_pending_ticket_publish_candidate(
                jc, ea, size,
                (targets.jobchain_bridges & (1u << i)) != 0,
                descriptor_out);
        }
    }
    return -1;
}
#endif

static void jobchain_notify_guest_write(u32 ea, u32 size, int ppu_store,
                                        u32 command_mask, u32 bridge_mask)
{
    const u64 first = ea;
    const u64 last = first + size;
    u32 routed = command_mask | bridge_mask;
    while (routed) {
        u64 routed64 = routed;
        const u32 i = guest_write_take_bit64(&routed64);
        routed &= ~(1u << i);
        JobChainState* jc = &g_jobchains[i];
        if (!jc->sync.live) continue;
        const int in_command_window = (command_mask & (1u << i)) != 0;
        /* Cheap lock-free pre-filter: only a small store landing on a ring
         * descriptor's kind word with the pending value can be a bridge
         * candidate. Ordinary ring traffic (whole-slot builds at high rate)
         * must not take the chain mutex. */
        const int in_bridge_window = (bridge_mask & (1u << i)) != 0;
        u32 bridge_descriptor = 0;
        const int bridge_candidate =
            jobchain_pending_ticket_publish_candidate(
                jc, ea, size, in_bridge_window, &bridge_descriptor);
        if (!in_command_window && !bridge_candidate) continue;
        mx_lock(&jc->sync.mutex);
        int wake_transition = 0;
        u64 exact_publication_sequence = 0;
        int exact_polled_ack = 0;
        if (in_command_window && size == sizeof(u64) && !(ea & 7u) &&
            job_guest_range_valid(ea, sizeof(u64))) {
            const u64 exact_command = rd64(vm_base + ea);
            exact_polled_ack = jobchain_ack_polled_command(jc, ea, exact_command);
            if (!exact_polled_ack && (exact_command >> 32) == 0u &&
                (exact_command & 7u) == 4u &&
                jobchain_plausible_publication_head_locked(jc, ea))
                jobchain_supersede_pending_callee(
                    jc, (u32)(exact_command & ~7ull));
            /* Producers clear reusable circular slots explicitly.  Remember
             * every such slot, including ones the worker consumed without
             * ever parking on, so a later wrapped publication cannot start
             * behind current_ea unnoticed. */
            if (job_command_is_empty(ea, exact_command))
                jobchain_remember_parked_slot(jc, ea, exact_command);
            else if (!exact_polled_ack &&
                     (job_command_starts_work(exact_command) ||
                      job_command_is_publication_barrier(exact_command))) {
                int slot = jobchain_command_slot(jc, ea, 0);
                if (slot < 0 &&
                    jobchain_plausible_publication_head_locked(jc, ea))
                    slot = jobchain_command_slot(jc, ea, 1);
                if (slot >= 0)
                    exact_publication_sequence =
                        ++jc->command_publication_sequence[slot];
            }
        }
        /* Pending-ticket bridge. The title's synchronous loads submit a ring
         * ticket by writing kind 6 into descriptor+0x10, then spin until the
         * SPU side clears it; no job-chain command is appended for it. The
         * real kernel re-executes the stale JOB command still in the
         * circular list and the self-gating dispatcher picks the ticket up;
         * this scanner never revisits consumed commands.
         * Bridge: when a write to either readiness word leaves the complete
         * tuple { kind 6, size_io nonzero }, re-dispatch the command slot
         * that last dispatched that descriptor. A double dispatch is a
         * no-op: the dispatcher reads kind 0 and returns. */
        int bridge_resumed = 0;
        if (bridge_candidate) {
            /* Ring descriptors are 128-aligned; a small store completing the
             * +0x10/+0x14 tuple identifies a synchronous-submit ticket
             * structurally (no map to evict).
             * The stale JOB command for it persists in guest memory in one
             * of the command slots this chain has already seen. */
            if (ppu_store) {
                /* The descriptor header is deliberately written before the
                 * DMA list and job-specific body. Dispatching here can grab
                 * a stable but incomplete generation. The producer's PPU
                 * fence publishes this staged address below. */
                jobchain_stage_ppu_bridge_candidate(bridge_descriptor);
            } else {
                /* SPU DMA and runtime/HLE bulk writers notify only after the
                 * complete transfer, so their notification is itself the
                 * publication edge. */
                bridge_resumed = jobchain_resume_pending_ticket_locked(
                    jc, bridge_descriptor);
            }
        }
        wake_transition |= bridge_resumed;
        int exact_barrier_handled = 0;
        for (u32 p = 0; in_command_window && p < jc->parked_slot_count; ++p) {
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
            if ((slot_ea == ea && exact_polled_ack) ||
                jobchain_ack_polled_command(jc, slot_ea, command)) {
                if (slot_ea == ea && job_command_is_publication_barrier(command))
                    exact_barrier_handled = 1;
                continue;
            }
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
                const u64 resume_sequence = jobchain_defer_resume(
                    jc, slot_ea, publication_sequence);
                if (size == sizeof(u64) && slot_ea == ea &&
                    exact_publication_sequence && job_command_is_job(command))
                    queued = jobchain_queue_published_job(jc, slot_ea,
                        (u32)command, publication_sequence, resume_sequence);
                if (job_command_is_publication_barrier(command))
                    queued = jobchain_queue_snapshot(
                        jc, slot_ea, /*replace_unconsumed=*/0);
            } else if (job_command_is_publication_barrier(command)) {
                /* Future publications must be claimed before the producer is
                 * allowed to reuse their body.  Ordinary NEXT/JOB maintenance
                 * is not a publication head and must not consume the bounded
                 * pending-snapshot slots. */
                queued = jobchain_queue_snapshot(
                    jc, slot_ea, /*replace_unconsumed=*/1);
            }
            wake_transition |= claimed != 0;
            wake_transition |= queued != 0 && jc->waiting_command;
        }
        /* The first publication of a circular slot can race ahead of the
         * native worker ever parking there.  Claim a complete synchronization
         * command only at a known command slot or the immediate successor of
         * one.  The broad watch envelope can span distant CALL lists; treating
         * every value 2/10/18/26 inside that gap as a barrier would queue
         * unrelated title data. */
        if (!exact_barrier_handled && size == sizeof(u64) && !(ea & 7u) &&
            job_guest_range_valid(ea, sizeof(u64)) &&
            jobchain_plausible_publication_head_locked(jc, ea)) {
            const u64 command = rd64(vm_base + ea);
            if (job_command_is_publication_barrier(command)) {
                const int command_slot = jobchain_command_slot(jc, ea, 1);
                const int reused = command_slot >= 0 ?
                    atomic_exchange_explicit(
                        &jc->command_consumed[command_slot], 0,
                        memory_order_acq_rel) : 0;
                const u32 queued = jobchain_queue_snapshot(
                    jc, ea, /*replace_unconsumed=*/!reused);
                wake_transition |= queued != 0 && jc->waiting_command;
            }
        }
        /* A PPU bridge-only store is staged until its publishing fence, which
         * performs the wake. Completed non-PPU publications wake here. */
        if (wake_transition)
            cv_wake_all(&jc->sync.cond);
        mx_unlock(&jc->sync.mutex);
    }
}

void cellSpursNotifyPpuGuestWrite(u32 ea, u32 size)
{
    cellSpursNotifyGuestWriteSource(ea, size, 1);
}

void cellSpursNotifyPpuFence(void)
{
    const u32 count = g_ppu_bridge_candidate_count;
    if (!count) return;
    u32 candidate[MAX_PPU_BRIDGE_CANDIDATES];
    memcpy(candidate, g_ppu_bridge_candidate, count * sizeof(candidate[0]));
    g_ppu_bridge_candidate_count = 0;
    atomic_thread_fence(memory_order_seq_cst);
    u32 routed = 0;
    for (u32 p = 0; p < count; ++p) {
        GuestWriteTargets targets;
        if (guest_write_route_lookup(candidate[p] + 0x10u, 8u, &targets))
            routed |= targets.jobchain_bridges;
    }
    while (routed) {
        u64 routed64 = routed;
        const u32 i = guest_write_take_bit64(&routed64);
        routed &= ~(1u << i);
        JobChainState* jc = &g_jobchains[i];
        if (!jc->sync.live) continue;
        int resumed = 0;
        mx_lock(&jc->sync.mutex);
        for (u32 p = 0; p < count; ++p) {
            GuestWriteTargets targets;
            guest_write_route_lookup(candidate[p] + 0x10u, 8u, &targets);
            if (targets.jobchain_bridges & (1u << i))
                resumed |= jobchain_resume_pending_ticket_locked(
                    jc, candidate[p]) != 0;
        }
        if (resumed) cv_wake_all(&jc->sync.cond);
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

/* Test support: report whether a jobchain's worker is parked
 * waiting for a command at guest EA `ea`. The claimed-SYNC publication
 * regression test needs this deterministic parked edge before writing its
 * exact SYNC; a fixed sleep would race the worker. Not part of any SDK
 * surface. */
int yz_spurs_jobchain_is_parked_at(CellSpursJobChain* object, u32 ea)
{
    JobChainState* jc = jobchain_find(object);
    if (!jc) return 0;
    mx_lock(&jc->sync.mutex);
    const int parked = jc->waiting_command && jc->current_ea == ea;
    mx_unlock(&jc->sync.mutex);
    return parked;
}
#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
int yz_spurs_test_pending_snapshot_count(CellSpursJobChain* object)
{
    JobChainState* jc = jobchain_find(object);
    if (!jc) return -1;
    int count = 0;
    mx_lock(&jc->sync.mutex);
    for (PendingJobSnapshot* snapshot = jc->pending_snapshot_head;
         snapshot; snapshot = snapshot->next)
        ++count;
    mx_unlock(&jc->sync.mutex);
    return count;
}
#endif
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

static void jobguard_notify_guest_write(u32 ea, u32 size, u64 mask)
{
    const u64 first = ea;
    const u64 last = first + size;
    while (mask) {
        const u32 i = guest_write_take_bit64(&mask);
        JobGuardState* g = &g_jobguards[i];
        if (!g->sync.live || !g->sync.key) continue;
        const u64 guard_ea = guest_ea(g->sync.key);
        if (first >= guard_ea + 4u || last <= guard_ea) continue;
        mx_lock(&g->sync.mutex);
        const u32 previous = g->count;
        g->count = rd32(g->sync.key);
        if (previous && !g->count)
            cv_wake_all(&g->sync.cond);
        mx_unlock(&g->sync.mutex);
    }
}
