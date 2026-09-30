/*
 * Firmware-free, host-native SPURS scheduler.
 *
 * The implementation deliberately keeps guest object bytes authoritative.
 * Host locks, condition variables and workers are held in bounded side tables
 * keyed by the guest object pointer.
 */
#include "cellSpurs.h"
#include "gs_idle_journal.h"
#include "../../runtime/spu/spu_workload.h"
#include "../../runtime/spu/spu_job_dispatch.h"
#include "../../runtime/memory/vm.h"
#include "ps3emu/yz_thread_placement.h"
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
#define NATIVE_SPU_HOST_STACK_SIZE (64u * 1024u * 1024u)
static void mx_init(nmutex* m) { InitializeCriticalSection(m); }
static void mx_destroy(nmutex* m) { DeleteCriticalSection(m); }
static void mx_lock(nmutex* m) { EnterCriticalSection(m); }
static void mx_unlock(nmutex* m) { LeaveCriticalSection(m); }
static void cv_init(ncond* c) { InitializeConditionVariable(c); }
static void cv_wait(ncond* c, nmutex* m) { SleepConditionVariableCS(c, m, INFINITE); }
/* Bounded wait for parks whose wake depends on a guest-memory store. Vector
 * stores and raw memcpys in lifted code bypass the vm_write notify hook, so a
 * producer may never signal the watcher; a timed re-check bounds that
 * latency. Predicates are re-read on every wake, so spurious wakeups are
 * harmless. */
static void cv_wait_ms(ncond* c, nmutex* m, unsigned ms)
{ SleepConditionVariableCS(c, m, ms); }
static void cv_wake_all(ncond* c) { WakeAllConditionVariable(c); }
static void nthread_yield(void) { SwitchToThread(); }
static void nthread_reschedule(void) { Sleep(1); }
static int nthread_create_spu(nthread* thread, LPTHREAD_START_ROUTINE entry, void* opaque)
{
    /*
     * Lifted SPU functions retain host call frames.  Reserve a large stack so
     * task and job workers can run those functions without exhausting the
     * platform's small default thread stack.
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
#define NATIVE_SPU_HOST_STACK_SIZE (64u * 1024u * 1024u)
static void mx_init(nmutex* m) { pthread_mutex_init(m, NULL); }
static void mx_destroy(nmutex* m) { pthread_mutex_destroy(m); }
static void mx_lock(nmutex* m) { pthread_mutex_lock(m); }
static void mx_unlock(nmutex* m) { pthread_mutex_unlock(m); }
static void cv_init(ncond* c) { pthread_cond_init(c, NULL); }
static void cv_wait(ncond* c, nmutex* m) { pthread_cond_wait(c, m); }
static void cv_wait_ms(ncond* c, nmutex* m, unsigned ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    ts.tv_sec += ms / 1000u + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(c, m, &ts);
}
static void cv_wake_all(ncond* c) { pthread_cond_broadcast(c); }
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


/* Journal word the GS task is parked on in gs_idle_wait, or 0. */
static atomic_uint g_gs_idle_wait_word;

#if defined(_WIN32)
volatile LONG g_yz_jobchain_bridge_success_count;
static void jobchain_note_bridge_success(void)
{
    InterlockedIncrement(&g_yz_jobchain_bridge_success_count);
}
#else
volatile long g_yz_jobchain_bridge_success_count;
static void jobchain_note_bridge_success(void)
{
    __atomic_add_fetch(&g_yz_jobchain_bridge_success_count, 1,
                       __ATOMIC_RELEASE);
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




/* ------------------------------------------------------------------------- */
/* Side-table synchronization registry                                       */

#define MAX_SPURS_INSTANCES 16
#define MAX_TASKSETS 64
#define MAX_EVENT_FLAGS 128
#define MAX_QUEUES 128
#define MAX_JOBCHAINS 32
#define MAX_JOB_GUARDS 64

/* Exact guest-write routing.
 *
 * VM writes are overwhelmingly unrelated to SPURS.  A single process-wide
 * min/max envelope followed by a linear scan of every side table would be too
 * costly: a distant CALL target widens the envelope across all memory between
 * the two command lists, so ordinary RSX register writes would pay for every
 * SPURS registry and frequently wake every jobchain worker.
 *
 * This table maps each watched eight-byte guest cell directly to one live
 * side-table slot.  Entries are append-only and lock-free for readers.  A
 * target generation makes old entries inert when a bounded side-table slot is
 * recycled or a jobchain starts a fresh run.  The page bitmap is only the
 * inlined VM-hook fast reject; an address on a populated page still has to
 * match an exact router entry below. */
#define GUEST_WRITE_ROUTE_CAPACITY (1u << 20)
#define GUEST_WRITE_ROUTE_MASK (GUEST_WRITE_ROUTE_CAPACITY - 1u)
#define GUEST_WRITE_PAGE_SHIFT 12u
#define GUEST_WRITE_PAGE_COUNT (1u << (32u - GUEST_WRITE_PAGE_SHIFT))
#define GUEST_WRITE_PAGE_WORDS (GUEST_WRITE_PAGE_COUNT / 64u)

enum GuestWriteRouteKind {
    GUEST_WRITE_ROUTE_TASKSET = 1,
    GUEST_WRITE_ROUTE_EVENT = 2,
    GUEST_WRITE_ROUTE_QUEUE = 3,
    GUEST_WRITE_ROUTE_JOBGUARD = 4,
    GUEST_WRITE_ROUTE_JOBCHAIN_COMMAND = 5,
    GUEST_WRITE_ROUTE_JOBCHAIN_BRIDGE = 6,
    GUEST_WRITE_ROUTE_KIND_COUNT = 7
};

typedef struct {
    u64 tasksets;
    u64 events[2];
    u64 queues[2];
    u64 jobguards;
    u32 jobchain_commands;
    u32 jobchain_bridges;
} GuestWriteTargets;

static _Atomic u64 g_guest_write_route[GUEST_WRITE_ROUTE_CAPACITY];
static _Atomic u32 g_guest_write_generation[GUEST_WRITE_ROUTE_KIND_COUNT][128];
static _Atomic int g_guest_write_route_overflow;

typedef struct GuestWriteRouteOverflow {
    u64 encoded;
    struct GuestWriteRouteOverflow* next;
} GuestWriteRouteOverflow;

/* Saturation is not expected in normal play, but it must be correctness-safe.
 * Once the fixed, cache-friendly table fills, registrations move to this
 * append-only exact list. Readers take an atomic snapshot of the head and
 * never need a lock because nodes are immutable and live until process exit. */
static _Atomic(GuestWriteRouteOverflow*) g_guest_write_route_overflow_head;

/* Read directly by the lifted-PPU VM write hooks.  Bits are monotonic: stale
 * pages can cause one exact lookup, but can never route a stale target because
 * the generation check below rejects it. */
#if defined(_WIN32)
__declspec(align(64)) volatile u64
    g_native_spurs_watch_page_bits[GUEST_WRITE_PAGE_WORDS];
#else
_Alignas(64) volatile u64
    g_native_spurs_watch_page_bits[GUEST_WRITE_PAGE_WORDS];
#endif

static u32 guest_write_route_hash(u32 cell)
{
    u32 x = cell;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x & GUEST_WRITE_ROUTE_MASK;
}

static u32 guest_write_route_begin_target(u32 kind, u32 index)
{
    u32 generation = atomic_fetch_add_explicit(
        &g_guest_write_generation[kind][index], 1u,
        memory_order_acq_rel) + 1u;
    /* Zero is reserved for an unregistered target.  A 16-bit wrap would need
     * 65,535 lifecycles of the same bounded slot in one process. */
    if (!(generation & 0xffffu))
        generation = atomic_fetch_add_explicit(
            &g_guest_write_generation[kind][index], 1u,
            memory_order_acq_rel) + 1u;
    return generation & 0xffffu;
}

static u32 guest_write_route_target_generation(u32 kind, u32 index)
{
    return atomic_load_explicit(
        &g_guest_write_generation[kind][index], memory_order_acquire) & 0xffffu;
}

static void guest_write_route_mark_page(u32 ea)
{
    const u32 page = ea >> GUEST_WRITE_PAGE_SHIFT;
    const u32 word = page >> 6;
    const u64 bit = 1ull << (page & 63u);
#if defined(_WIN32)
    InterlockedOr64((volatile LONG64*)&g_native_spurs_watch_page_bits[word],
                    (LONG64)bit);
#else
    __atomic_fetch_or(&g_native_spurs_watch_page_bits[word], bit,
                      __ATOMIC_RELEASE);
#endif
}

static u64 guest_write_route_page_word(u32 word)
{
    return atomic_load_explicit(
        (const _Atomic u64*)&g_native_spurs_watch_page_bits[word],
        memory_order_relaxed);
}

static void guest_write_route_overflow_add(u64 encoded)
{
    GuestWriteRouteOverflow* node =
        (GuestWriteRouteOverflow*)malloc(sizeof(*node));
    if (!node) {
        /* Missing a registered wake can deadlock the guest. An explicit hard
         * failure is safer than continuing with silently incomplete routing. */
        fprintf(stderr,
                "[native-spurs] exact guest-write router overflow allocation "
                "failed; refusing to run with incomplete notifications\n");
        fflush(stderr);
        abort();
    }
    node->encoded = encoded;
    GuestWriteRouteOverflow* head = atomic_load_explicit(
        &g_guest_write_route_overflow_head, memory_order_acquire);
    for (;;) {
        /* Re-check on every failed CAS. Two registering threads can discover
         * the same missing cell concurrently; only one immutable node should
         * survive or a repeatedly watched saturated cell would grow forever. */
        for (GuestWriteRouteOverflow* existing = head;
             existing; existing = existing->next) {
            if (existing->encoded == encoded) {
                free(node);
                return;
            }
        }
        node->next = head;
        if (atomic_compare_exchange_weak_explicit(
                &g_guest_write_route_overflow_head, &head, node,
                memory_order_release, memory_order_acquire))
            break;
    }
    if (!atomic_exchange_explicit(&g_guest_write_route_overflow, 1,
                                  memory_order_release)) {
        fprintf(stderr,
                "[native-spurs] exact guest-write router saturated; "
                "using exact overflow routing\n");
        fflush(stderr);
    }
}

static void guest_write_route_watch_cell(u32 kind, u32 index,
                                         u32 generation, u32 cell)
{
    const u32 key = cell + 1u;
    const u32 token = ((generation & 0xffffu) << 16) |
                      ((kind & 0xffu) << 8) | (index & 0xffu);
    const u64 encoded = ((u64)key << 32) | token;
    u32 slot = guest_write_route_hash(cell);
    for (u32 probe = 0; probe < GUEST_WRITE_ROUTE_CAPACITY; ++probe) {
        u64 existing = atomic_load_explicit(
            &g_guest_write_route[slot], memory_order_acquire);
        if (existing == encoded) return;
        if (!existing) {
            if (atomic_compare_exchange_weak_explicit(
                    &g_guest_write_route[slot], &existing, encoded,
                    memory_order_release, memory_order_acquire))
                return;
            if (existing == encoded) return;
        }
        slot = (slot + 1u) & GUEST_WRITE_ROUTE_MASK;
    }
    guest_write_route_overflow_add(encoded);
}

static void guest_write_route_watch(u32 kind, u32 index, u32 ea, u32 size)
{
    if (!size || kind == 0 || kind >= GUEST_WRITE_ROUTE_KIND_COUNT ||
        index >= 128u)
        return;
    const u32 generation = guest_write_route_target_generation(kind, index);
    if (!generation) return;
    const u64 last64 = (u64)ea + size - 1u;
    const u32 last = last64 > UINT32_MAX ? UINT32_MAX : (u32)last64;
    const u32 first_cell = ea >> 3;
    const u32 last_cell = last >> 3;
    for (u32 cell = first_cell; ; ++cell) {
        guest_write_route_watch_cell(kind, index, generation, cell);
        guest_write_route_mark_page(cell << 3);
        if (cell == last_cell) break;
    }
}

static void guest_write_targets_add(GuestWriteTargets* targets,
                                    u32 kind, u32 index)
{
    switch (kind) {
    case GUEST_WRITE_ROUTE_TASKSET:
        targets->tasksets |= 1ull << index;
        break;
    case GUEST_WRITE_ROUTE_EVENT:
        targets->events[index >> 6] |= 1ull << (index & 63u);
        break;
    case GUEST_WRITE_ROUTE_QUEUE:
        targets->queues[index >> 6] |= 1ull << (index & 63u);
        break;
    case GUEST_WRITE_ROUTE_JOBGUARD:
        targets->jobguards |= 1ull << index;
        break;
    case GUEST_WRITE_ROUTE_JOBCHAIN_COMMAND:
        targets->jobchain_commands |= 1u << index;
        break;
    case GUEST_WRITE_ROUTE_JOBCHAIN_BRIDGE:
        targets->jobchain_bridges |= 1u << index;
        break;
    default:
        break;
    }
}

static int guest_write_route_lookup(u32 ea, u32 size,
                                    GuestWriteTargets* targets)
{
    memset(targets, 0, sizeof(*targets));
    if (!size) return 0;
    const u64 last64 = (u64)ea + size - 1u;
    const u32 last = last64 > UINT32_MAX ? UINT32_MAX : (u32)last64;
    const u32 first_page = ea >> GUEST_WRITE_PAGE_SHIFT;
    const u32 last_page = last >> GUEST_WRITE_PAGE_SHIFT;
    int matched = 0;
    for (u32 page = first_page; ; ++page) {
        const u64 page_word = guest_write_route_page_word(page >> 6);
        if (page_word & (1ull << (page & 63u))) {
            const u32 page_first_cell = page << (GUEST_WRITE_PAGE_SHIFT - 3u);
            const u32 page_last_cell = page_first_cell +
                ((1u << (GUEST_WRITE_PAGE_SHIFT - 3u)) - 1u);
            const u32 first_cell = page == first_page ?
                ea >> 3 : page_first_cell;
            const u32 last_cell = page == last_page ?
                last >> 3 : page_last_cell;
            for (u32 cell = first_cell; ; ++cell) {
                const u32 key = cell + 1u;
                u32 slot = guest_write_route_hash(cell);
                for (u32 probe = 0; probe < GUEST_WRITE_ROUTE_CAPACITY;
                     ++probe) {
                    const u64 encoded = atomic_load_explicit(
                        &g_guest_write_route[slot], memory_order_acquire);
                    if (!encoded) break;
                    if ((u32)(encoded >> 32) == key) {
                        const u32 token = (u32)encoded;
                        const u32 kind = (token >> 8) & 0xffu;
                        const u32 index = token & 0xffu;
                        const u32 generation = token >> 16;
                        if (kind < GUEST_WRITE_ROUTE_KIND_COUNT &&
                            index < 128u && generation ==
                                guest_write_route_target_generation(
                                    kind, index)) {
                            guest_write_targets_add(targets, kind, index);
                            matched = 1;
                        }
                    }
                    slot = (slot + 1u) & GUEST_WRITE_ROUTE_MASK;
                }
                if (atomic_load_explicit(&g_guest_write_route_overflow,
                                         memory_order_acquire)) {
                    const u32 key = cell + 1u;
                    const GuestWriteRouteOverflow* overflow =
                        atomic_load_explicit(
                            &g_guest_write_route_overflow_head,
                            memory_order_acquire);
                    for (; overflow; overflow = overflow->next) {
                        const u64 encoded = overflow->encoded;
                        if ((u32)(encoded >> 32) != key) continue;
                        const u32 token = (u32)encoded;
                        const u32 kind = (token >> 8) & 0xffu;
                        const u32 index = token & 0xffu;
                        const u32 generation = token >> 16;
                        if (kind < GUEST_WRITE_ROUTE_KIND_COUNT &&
                            index < 128u && generation ==
                                guest_write_route_target_generation(
                                    kind, index)) {
                            guest_write_targets_add(targets, kind, index);
                            matched = 1;
                        }
                    }
                }
                if (cell == last_cell) break;
            }
        }
        if (page == last_page) break;
    }
    return matched;
}

#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
static u32 guest_write_route_popcount64(u64 value)
{
    u32 count = 0;
    while (value) {
        value &= value - 1u;
        ++count;
    }
    return count;
}

int yz_spurs_test_guest_write_route_count(u32 ea, u32 size)
{
    GuestWriteTargets targets;
    if (!guest_write_route_lookup(ea, size, &targets)) return 0;
    return (int)(guest_write_route_popcount64(targets.tasksets) +
                 guest_write_route_popcount64(targets.events[0]) +
                 guest_write_route_popcount64(targets.events[1]) +
                 guest_write_route_popcount64(targets.queues[0]) +
                 guest_write_route_popcount64(targets.queues[1]) +
                 guest_write_route_popcount64(targets.jobguards) +
                 guest_write_route_popcount64(targets.jobchain_commands) +
                 guest_write_route_popcount64(targets.jobchain_bridges));
}

int yz_spurs_test_guest_write_route_overflow_alias(u32 source_ea, u32 alias_ea)
{
    const u32 source_cell = source_ea >> 3;
    const u32 source_key = source_cell + 1u;
    u32 slot = guest_write_route_hash(source_cell);
    for (u32 probe = 0; probe < GUEST_WRITE_ROUTE_CAPACITY; ++probe) {
        const u64 encoded = atomic_load_explicit(
            &g_guest_write_route[slot], memory_order_acquire);
        if (!encoded) return -1;
        if ((u32)(encoded >> 32) == source_key) {
            const u32 token = (u32)encoded;
            const u32 kind = (token >> 8) & 0xffu;
            const u32 index = token & 0xffu;
            const u32 generation = token >> 16;
            if (kind < GUEST_WRITE_ROUTE_KIND_COUNT && index < 128u &&
                generation ==
                    guest_write_route_target_generation(kind, index)) {
                const u64 alias =
                    ((u64)((alias_ea >> 3) + 1u) << 32) | token;
                guest_write_route_overflow_add(alias);
                guest_write_route_mark_page(alias_ea);
                return 0;
            }
        }
        slot = (slot + 1u) & GUEST_WRITE_ROUTE_MASK;
    }
    return -1;
}
#endif

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

/* The SPURS implementation is one translation unit: the parts below share
 * the state above and are included once, in dependency order. */
#include "spurs_taskset.inc"
#include "spurs_event_flag.inc"
#include "spurs_queue.inc"
#include "spurs_jobchain_state.inc"
#include "spurs_jobchain_api.inc"
#include "spurs_jobchain_exec.inc"
