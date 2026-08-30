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
#include "ps3emu/yz_frontier_trace.h"
#include "ps3emu/yz_fe0_timeline.h"
#include "ps3emu/yz_frame_dependency_timeline.h"
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
static SRWLOCK g_unknown_job_capture_mutex = SRWLOCK_INIT;
static void unknown_job_capture_lock(void) { AcquireSRWLockExclusive(&g_unknown_job_capture_mutex); }
static void unknown_job_capture_unlock(void) { ReleaseSRWLockExclusive(&g_unknown_job_capture_mutex); }
static void mx_init(nmutex* m) { InitializeCriticalSection(m); }
static void mx_destroy(nmutex* m) { DeleteCriticalSection(m); }
static void mx_lock(nmutex* m) { EnterCriticalSection(m); }
static void mx_unlock(nmutex* m) { LeaveCriticalSection(m); }
static void cv_init(ncond* c) { InitializeConditionVariable(c); }
static void cv_wait(ncond* c, nmutex* m) { SleepConditionVariableCS(c, m, INFINITE); }
/* Bounded wait for parks whose wake depends on OBSERVING a guest-memory
 * store. Lifted vector (stvx) stores and raw lifted-PRX memcpys bypass the
 * vm_write* notify hook, so a producer can append a jobchain command without
 * ever signalling the watcher. A timed re-check converts that missed notify
 * from a permanent park (the Akiyama/Hana effect-stream hang) into bounded
 * latency; predicates are re-read on every wake, so spurious wakeups are
 * harmless. Remove when the relift emits notifying stores for all widths. */
static void cv_wait_ms(ncond* c, nmutex* m, unsigned ms)
{ SleepConditionVariableCS(c, m, ms); }
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
static pthread_mutex_t g_unknown_job_capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static void unknown_job_capture_lock(void) { pthread_mutex_lock(&g_unknown_job_capture_mutex); }
static void unknown_job_capture_unlock(void) { pthread_mutex_unlock(&g_unknown_job_capture_mutex); }
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

#if !defined(YZ_PERF_CLEAN)
static FILE* g_jobdone_diag_output;
static char g_jobdone_diag_buffer[256u * 1024u];

static void jobdone_diag_close(void)
{
    if (!g_jobdone_diag_output) return;
    fclose(g_jobdone_diag_output);
    g_jobdone_diag_output = NULL;
}
#endif

void cellSpursRuntimeConfigInit(void)
{
#if !defined(YZ_PERF_CLEAN)
    const char* path;
    if (g_jobdone_diag_output) return;
    path = getenv("YZ_NATIVE_SPURS_JOBDONE_LOG");
    if (!path || !path[0]) return;
    g_jobdone_diag_output = fopen(path, "w");
    if (!g_jobdone_diag_output) {
        fprintf(stderr,
                "[jobdone] cannot open YZ_NATIVE_SPURS_JOBDONE_LOG: %s\n",
                path);
        return;
    }
    setvbuf(g_jobdone_diag_output, g_jobdone_diag_buffer, _IOFBF,
            sizeof(g_jobdone_diag_buffer));
    atexit(jobdone_diag_close);
#endif
}

#if defined(_WIN32)
volatile LONG g_yz_frontier_bridge_success_count;
static void jobchain_note_bridge_success(void)
{
    InterlockedIncrement(&g_yz_frontier_bridge_success_count);
}
#else
volatile long g_yz_frontier_bridge_success_count;
static void jobchain_note_bridge_success(void)
{
    __atomic_add_fetch(&g_yz_frontier_bridge_success_count, 1,
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

static int jobchain_bridge_disabled(void)
{
    static int disabled = -1;
    if (disabled < 0)
        disabled = getenv("YZ_NO_JC_BRIDGE") ? 1 : 0;
    return disabled;
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

/* Persist exact unknown-job identities for the next build's inventory gate.
 * The runner sets YZ_SPU_INVENTORY_CAPTURE beside the game inputs, so normal
 * boots require no special logging command.  A bounded atomic set suppresses
 * duplicate worker reports in one process; the existing file is checked too
 * so repeated boots do not grow it indefinitely. */
#define MAX_UNKNOWN_JOB_CAPTURES 64
static atomic_ullong g_unknown_job_capture_keys[MAX_UNKNOWN_JOB_CAPTURES];

static int unknown_job_capture_first(u32 binary_ea, u32 binary_size, u64 fingerprint)
{
    u64 key = fingerprint ^ ((u64)binary_ea << 32) ^ binary_size;
    if (!key) key = 1;
    u32 index = (u32)(key ^ (key >> 32)) % MAX_UNKNOWN_JOB_CAPTURES;
    for (u32 probe = 0; probe < MAX_UNKNOWN_JOB_CAPTURES; ++probe) {
        atomic_ullong* slot = &g_unknown_job_capture_keys[
            (index + probe) % MAX_UNKNOWN_JOB_CAPTURES];
        unsigned long long seen = atomic_load_explicit(slot, memory_order_acquire);
        if ((u64)seen == key) return 0;
        if (!seen) {
            unsigned long long empty = 0;
            if (atomic_compare_exchange_strong_explicit(
                    slot, &empty, (unsigned long long)key,
                    memory_order_acq_rel, memory_order_acquire))
                return 1;
        }
    }
    return 1; /* table saturation must not lose evidence */
}

static int unknown_job_capture_file_has(
    const char* path, u32 binary_ea, u32 binary_size, u64 fingerprint)
{
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    char ea_token[48], size_token[48], fingerprint_token[64], line[1024];
    snprintf(ea_token, sizeof(ea_token),
             "\"binary_ea\":\"0x%08X\"", binary_ea);
    snprintf(size_token, sizeof(size_token),
             "\"binary_size\":\"0x%X\"", binary_size);
    snprintf(fingerprint_token, sizeof(fingerprint_token),
             "\"fingerprint\":\"0x%016llX\"",
             (unsigned long long)fingerprint);
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, ea_token) && strstr(line, size_token) &&
            strstr(line, fingerprint_token)) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

static int unknown_job_capture_write(
    const char* path, u32 descriptor_ea, u32 descriptor_size,
    u32 binary_ea, u32 binary_size, u64 fingerprint, u32 next_slot)
{
    if (!path || !*path) return 0;
    unknown_job_capture_lock();
    if (!unknown_job_capture_first(binary_ea, binary_size, fingerprint)) {
        unknown_job_capture_unlock();
        return 0;
    }
    if (unknown_job_capture_file_has(path, binary_ea, binary_size, fingerprint)) {
        unknown_job_capture_unlock();
        return 0;
    }
    FILE* file = fopen(path, "ab");
    if (!file) {
        unknown_job_capture_unlock();
        return -1;
    }
    const int rc = fprintf(file,
        "{\"schema\":1,\"kind\":\"native_spurs_unknown_job\","
        "\"binary_ea\":\"0x%08X\",\"binary_size\":\"0x%X\","
        "\"descriptor_ea\":\"0x%08X\",\"descriptor_size\":\"0x%X\","
        "\"fingerprint\":\"0x%016llX\",\"next_slot\":\"0x%05X\"}\n",
        binary_ea, binary_size, descriptor_ea, descriptor_size,
        (unsigned long long)fingerprint, next_slot);
    const int close_rc = fclose(file);
    unknown_job_capture_unlock();
    return rc > 0 && close_rc == 0 ? 1 : -1;
}

static void unknown_job_capture(
    u32 descriptor_ea, u32 descriptor_size, u32 binary_ea,
    u32 binary_size, u64 fingerprint, u32 next_slot)
{
    const char* path = getenv("YZ_SPU_INVENTORY_CAPTURE");
    if (!path || !*path) return;
    const int rc = unknown_job_capture_write(
        path, descriptor_ea, descriptor_size, binary_ea,
        binary_size, fingerprint, next_slot);
    if (rc > 0) {
        fprintf(stderr, "[spu-inventory] captured unknown job -> %s\n", path);
    } else if (rc < 0) {
        fprintf(stderr, "[spu-inventory] WARNING: could not append %s\n", path);
    }
    fflush(stderr);
}

#if defined(YZ_SPURS_DESCRIPTOR_SNAPSHOT_TEST)
int cellSpursTestCaptureUnknownJob(
    const char* path, u32 descriptor_ea, u32 descriptor_size,
    u32 binary_ea, u32 binary_size, u64 fingerprint, u32 next_slot)
{
    return unknown_job_capture_write(
        path, descriptor_ea, descriptor_size, binary_ea,
        binary_size, fingerprint, next_slot);
}
#endif

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
 * VM writes are overwhelmingly unrelated to SPURS.  The old notification
 * path first used one process-wide min/max envelope and then linearly scanned
 * every side table.  A distant CALL target widened that envelope across all
 * memory between the two command lists, so ordinary RSX register writes paid
 * for every SPURS registry and frequently woke every jobchain worker.
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
    /* Diagnostic provenance for the currently latched host signal.  Zero is
     * the ordinary user-callback SendSignal path; 4 is the exact event-edge
     * bridge, 5 is an exact taskset guest-latch write, and 6 is the HLE
     * LFQueue waiter bridge.  It never participates in the wake predicate. */
    u32 signal_origin;
    int exit_code;
    u32 exit_container_ea;   /* guest CellSpursTaskExitCode (0 = none) */
    unsigned idle_poll_count;
    unsigned trace_syscalls;
    unsigned profile_poll_workload_hits;
    unsigned profile_yield_calls;
    u32 fe0_timeline_cause;
    u32 fe0_timeline_epoch;
    u32 fe0_barrier_wait_count;
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
static void event_observe_native_wait(TasksetState* ts, TaskState* task,
                                      u32 candidate_ea);

/* Frozen-ticket forensics (2026-08-05, boots 3+10): one call dumps every
 * live taskset's guest state bitsets + host task flags so a stuck cellSync
 * ticket holder (SPU-side lifted code that stopped being scheduled
 * mid-critical-section) can be NAMED instead of inferred. Called from
 * cellSync's TryLock spin diagnostic when serving freezes. Racy reads by
 * design (point-in-time forensics on a wedged process). */
void yz_spurs_dump_tasksets(const char* tag)
{
    fprintf(stderr, "[ts-dump] %s: live tasksets:\n", tag ? tag : "");
    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        TasksetState* ts = &g_tasksets[i];
        if (!ts->sync.live) continue;
        const u8* o = (const u8*)ts->sync.key;
        fprintf(stderr,
                "[ts-dump]  obj=0x%08X wid=%u shut=%d run=%02X%02X ready=%02X%02X "
                "en=%02X%02X sig=%02X%02X wait=%02X%02X tasks:",
                guest_ea((void*)ts->sync.key), ts->wid, ts->shutdown,
                o[0x00], o[0x01], o[0x10], o[0x11], o[0x30], o[0x31],
                o[0x40], o[0x41], o[0x50], o[0x51]);
        for (u32 t = 0; t < 128; ++t) {
            const TaskState* k = &ts->tasks[t];
            if (!k->thread_valid && !k->complete) continue;
            fprintf(stderr, " %u%s%s%s%s", t,
                    k->complete ? "C" : "", k->waiting ? "W" : "",
                    k->signalled ? "S" : "", k->thread_valid ? "" : "-");
        }
        fprintf(stderr, "\n");
        /* Face-B forensics (2026-08-06): a WAITING task's saved guest
         * context holds what it parked ON (yield-time registers). Dump the
         * head of each waiting task's context area so the wait object can
         * be decoded offline against the SDK task-context layout. */
        for (u32 t = 0; t < 128; ++t) {
            const TaskState* k = &ts->tasks[t];
            if (!k->thread_valid || !k->waiting || !k->context_ea) continue;
            if (!vm_base || !job_guest_range_valid(k->context_ea, 0x60))
                continue;
            fprintf(stderr,
                    "[ts-dump]   task %u ctx=0x%08X:", t, k->context_ea);
            for (u32 o = 0; o < 0x60; o += 4)
                fprintf(stderr, "%s%08X", (o & 15u) ? " " : " | ",
                        rd32(vm_base + k->context_ea + o));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

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
     * transactions.  Native task workers used to update its bitsets with a
     * raw host byte store while an SPU could own a reservation on the same
     * 128-byte line.  A later successful full-line PUTLLC could therefore
     * restore an old running/ready/waiting/signal byte and lose a barrier
     * wake.  Taskset lines become permanently marked by the first GETLLAR,
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
        /* A yield or WAIT_SIGNAL is an image-4 execution boundary, not a
         * polling event.  Record the exact guest return PC before context
         * save so a delayed FE0 publication can be attributed to the
         * task-side primitive that handed control back to SPURS. */
        if (t->image.image_id == 4) {
            yz_fe0_timeline_emit(
                YZ_FE0_EVENT_WKL4_HANDOFF,
                t->fe0_timeline_cause, t->id,
                t->fe0_timeline_epoch, ts->wid, op,
                ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        }
        /* Both scheduler handoffs save LS/context for another dispatch.
         * Complete all task DMA first so WAIT_SIGNAL cannot expose a partial
         * context any more than YIELD can. */
        spu_task_wait_all_dma(ctx);
        if (op == 2) {
            if (t->image.image_id == 4) {
                yz_fe0_timeline_emit(
                    YZ_FE0_EVENT_WKL4_WAIT_ABI,
                    t->fe0_timeline_cause, t->id,
                    ctx->gpr[81]._u32[0], ctx->gpr[81]._u32[1],
                    ctx->gpr[81]._u32[2], ctx->gpr[81]._u32[3]);
            }
            /* Image 4 preserves a CellSpursBarrier EA in r81 lane 1 across
             * WAIT_SIGNAL.  The live object proves the SDK layout: total at
             * +0x04 and owning taskset EA at +0x34.  Earlier diagnostics
             * misread lanes 2/3 as LFQueue geometry and consequently
             * registered a queue observer over the barrier.  Accept only the
             * structural barrier signature and retain bounded snapshots; the
             * other r81 lanes remain opaque task context. */
            const u32 event_eah = ctx->gpr[81]._u32[0];
            const u32 event_eal = ctx->gpr[81]._u32[1];
            if (g_yz_fe0_timeline_enabled &&
                t->image.image_id == 4 && ts->wid == 4 &&
                t->fe0_timeline_cause != 0u && event_eah == 0 &&
                !(event_eal & 127u) &&
                job_guest_range_valid(event_eal, 128u)) {
                const u8* barrier = vm_base + event_eal;
                const u32 total = rd32(barrier + 0x04u);
                const u32 taskset_ea = rd32(barrier + 0x34u);
                if (total != 0u && total <= 128u &&
                    taskset_ea == guest_ea(ts->sync.key)) {
                    yz_fe0_timeline_set_wkl4_barrier(
                        event_eal, taskset_ea);
                    yz_fe0_timeline_emit(
                        YZ_FE0_EVENT_WKL4_BARRIER_WAIT,
                        t->fe0_timeline_cause, t->id,
                        event_eal,
                        rd32(barrier + 0x00u), total,
                        rd32(barrier + 0x10u));
                    /* Full layout is diagnostic evidence only.  Sampling a
                     * complete line once per 256 waits keeps the ring bounded
                     * while retaining the phase and participant masks. */
                    if ((++t->fe0_barrier_wait_count & 0xffu) == 1u) {
                        for (u32 off = 0; off < 128u; off += 16u) {
                            yz_fe0_timeline_emit(
                                YZ_FE0_EVENT_WKL4_BARRIER_LAYOUT,
                                t->fe0_timeline_cause, off,
                                rd32(barrier + off + 0x00u),
                                rd32(barrier + off + 0x04u),
                                rd32(barrier + off + 0x08u),
                                rd32(barrier + off + 0x0cu));
                        }
                    }
                }
            }
            /* A real lifted EventFlagWait has only the MFC address pair in
             * r81.  Keep discovery for that ABI, but never reinterpret the
             * barrier-plus-context tuple as an event flag. */
            if (event_eah == 0 && ctx->gpr[81]._u32[2] == 0u &&
                ctx->gpr[81]._u32[3] == 0u)
                event_observe_native_wait(ts, t, event_eal);
        }
        task_save_context(t, ctx);
        spu_mfc_context_switch(ctx);
        mx_lock(&ts->sync.mutex);
        if (op == 1) {
            /* YIELD returns the task to READY while its host worker gives the
             * scheduler a chance to run another native workload. */
            task_bit(ts->sync.key, 0x00, t->id, 0);
            taskset_refresh_runnable_locked(ts);
            mx_unlock(&ts->sync.mutex);
#if defined(YZ_PERF_PROFILE) && !defined(YZ_PERF_CLEAN)
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
            /* The signal buffer is a depth-1 LATCH in the GUEST bitset
             * (+0x40): "If a signal has already been sent ... immediately
             * clear it and return" ORACLE(libspurs_Task-Reference p.47).
             * A signaler running as lifted SPU code writes ONLY the guest
             * bit — it never sets the host mirror t->signalled — so the
             * predicate must consume the guest latch too. Re-polling only
             * the host flag slept forever on lifted-side signals (the
             * CriSr cellSync-ticket freeze, 2026-08-04). */
            {
                const u8* obj = (const u8*)ts->sync.key;
                const u8 sig_mask = (u8)(0x80u >> (t->id & 7));
                const u32 sig_byte = 0x40 + t->id / 8;
                /* Handoff-ordering ring (STATUS 2026-08-06): park + wake
                 * edges, with the wake reason. No I/O on this path. */
                yz_frontier_trace_emit(YZ_FT_TASK_WAIT, ts->wid, t->id,
                                       guest_ea(ts->sync.key), 0u,
                                       obj[sig_byte], t->signalled ? 1u : 0u,
                                       0, 0);
                while (!t->signalled && !(obj[sig_byte] & sig_mask) &&
                       !ts->shutdown)
                    /* Timed: lifted-SPU signalers bypass the HLE wake. */
                    cv_wait_ms(&ts->sync.cond, &ts->sync.mutex, 2u);
                if (!ts->shutdown && t->image.image_id == 4) {
                    yz_fe0_timeline_emit(
                        YZ_FE0_EVENT_WKL4_WAKE,
                        t->fe0_timeline_cause, t->id,
                        t->fe0_timeline_epoch, ts->wid,
                        guest_ea(ts->sync.key),
                        (t->signalled ? 1u : 2u) |
                            (t->signal_origin << 8));
                }
                /* Witness (2026-08-05): a guest-latch-only wake means a
                 * lifted-SPU signaler delivered while the host flag never
                 * set -- the path the 08-04 fix opened. First 8 only. */
                if (!t->signalled && (obj[sig_byte] & sig_mask)) {
                    static unsigned long nglw = 0;
                    if (++nglw <= 8) {
                        fprintf(stderr, "[spurs-glatch] WAIT_SIGNAL woke on "
                                "guest latch id=%u taskset=0x%08X n=%lu\n",
                                t->id, guest_ea(ts->sync.key), nglw);
                        fflush(stderr);
                    }
                }
                yz_frontier_trace_emit(YZ_FT_TASK_WAIT, ts->wid, t->id,
                                       guest_ea(ts->sync.key),
                                       ts->shutdown ? 3u :
                                       (t->signalled ? 1u : 2u),
                                       obj[sig_byte], t->signalled ? 1u : 0u,
                                       0, 0);
            }
            t->signalled = 0;
            t->signal_origin = 0;
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
        if (t->image.image_id == 4) {
            ctx->fe0_timeline_cause = t->fe0_timeline_cause;
            ctx->fe0_timeline_epoch = t->fe0_timeline_epoch;
            ctx->fe0_timeline_task = t->id;
            yz_fe0_timeline_emit(
                YZ_FE0_EVENT_WKL4_RESUME,
                t->fe0_timeline_cause, t->id,
                t->fe0_timeline_epoch, ts->wid, t->spu_num, ctx->pc);
        }
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
#if defined(YZ_PERF_PROFILE) && !defined(YZ_PERF_CLEAN)
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
#if defined(YZ_PERF_PROFILE) && defined(_WIN32) && !defined(YZ_PERF_CLEAN)
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
    if (task->image.image_id == 4) {
        ctx->fe0_timeline_cause = task->fe0_timeline_cause;
        ctx->fe0_timeline_epoch = task->fe0_timeline_epoch;
        ctx->fe0_timeline_task = task->id;
    }
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

    yz_frame_dep_spurs_schedule(1u, (u32)task->image.image_id,
                                task->owner->wid, task->id);
    yz_frame_dep_spu_task_start((u32)task->image.image_id, task->spu_num,
                                task->id, ctx->pc);
    if (!spu_workload_execute(&task->image, ctx))
        task->exit_code = CELL_SPURS_TASK_ERROR_NOEXEC;
    yz_frame_dep_spu_task_complete((u32)task->image.image_id, task->spu_num,
                                   task->id, ctx->pc);
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
    /* 2026-08-05 (queue item E): publish the exit code into the guest
     * exit-code container, value first then the ready flag, so
     * cellSpursTaskExitCode(Try)Get observes a complete record. The old
     * code accepted the container at attr+0x40 and never wrote it
     * (TryGet answered BUSY forever). */
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
    /* Task-Reference p.180 / task_types.h: the only legal sizes are the
     * class-0 (0x1900) and class-1 (0x2900) taskset sizes. Only the lower
     * bound used to be checked, and create_taskset_common memsets the
     * caller's object with this value -- an oversized/garbage attribute
     * word was an unbounded guest-memory clear (2026-08-04 audit). */
    if (size != CELL_SPURS_TASKSET_SIZE && size != CELL_SPURS_TASKSET2_SIZE)
        return CELL_SPURS_TASK_ERROR_INVAL;
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
    {
        const u32 index = (u32)(ts - g_tasksets);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_TASKSET, index);
        /* Lifted tasks publish signals in this 128-bit field.  No other
         * taskset byte is a host-worker wake predicate. */
        guest_write_route_watch(GUEST_WRITE_ROUTE_TASKSET, index,
                                guest_ea(taskset) + 0x40u, 0x10u);
    }
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
    /* Validate the size word before create_taskset_common memsets the guest
     * object with it -- an uninitialized/foreign attribute otherwise turns
     * into an unbounded guest-memory clear (2026-08-04 audit; legal values
     * per task_types.h are exactly the class-0/class-1 sizes). */
    u32 tsize = rd32(a->bytes + 0x20);
    if (tsize != CELL_SPURS_TASKSET_SIZE && tsize != CELL_SPURS_TASKSET2_SIZE)
        return CELL_SPURS_TASK_ERROR_INVAL;
    return create_taskset_common(s, ts, tsize,
                                  rd64(a->bytes + 0x08), a->bytes + 0x10,
                                  rd32(a->bytes + 0x18),
                                  a->bytes[0x24] ? 1 : 0);
}
s32 cellSpursCreateTaskset2(CellSpurs* s, CellSpursTaskset2* ts,
                            const CellSpursTasksetAttribute2* a)
{
    u64 args = a ? rd64(a->bytes + 0x08) : 0;
    /* CellSpursTasksetAttribute2 is a TRANSPARENT struct (task_types.h:
     * revision@0, name@4, argTaskset@8, priority[8]@0x10, maxContention@
     * 0x18, enableClearLs int32 @0x1C, taskNameBuffer@0x20) -- the old read
     * at 0x24 landed in __reserved__ and always saw 0 (2026-08-04 audit;
     * 0x24 is only right for the opaque class-1 attribute). */
    return create_taskset_common(s, ts, sizeof(*ts), args,
                                  a ? a->bytes + 0x10 : NULL,
                                  a ? rd32(a->bytes + 0x18) : 8,
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
    /* 2026-08-05 (queue item E): Join finishes the taskset — release its
     * workload id and recycle the host slot. Previously neither happened,
     * so MAX_TASKSETS and the workload table were LIFETIME totals on the
     * v1 CreateTask+JoinTaskset path. A signal to a joined taskset now
     * answers STAT (taskset_find misses) — the faithful dead-object reply —
     * instead of latching into whatever recycled the slot. DestroyTaskset's
     * own release tail turns into a no-op (its taskset_find misses too). */
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
/* Boot 57 (flight recorder): the wid-4 wake broadcasts are NOT
 * EventFlagSet (zero event-set records in every window while broadcasts
 * flowed) — they come from another SendSignal path. Tag the origin so the
 * ring names the producer: 0=direct guest import, 1=eventflag-wake,
 * 2=queue-push consumer wake, 3=queue-pop producer wake,
 * 4=lifted-SPU exact event-flag publication. */
#if defined(_MSC_VER)
static __declspec(thread) int g_yz_sendsig_origin;
#else
static _Thread_local int g_yz_sendsig_origin;
#endif
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
    /* S10 (amended 2026-08-04): the signal latches into the guest signalled
     * bitset regardless of host thread bookkeeping — a task id snapshotted
     * by a producer must not lose its wake because the host TaskState was
     * recycled in between (depth-1 latch, ORACLE(libspurs_Task-Reference
     * p.34)). But firmware gates delivery on the task EXISTING: libsre
     * @0x020125D8 checks the guest state sets and returns SRCH 0x80410905
     * for a dead id ("Specified task does not exist", p.34). Without the
     * gate, a stale waiter record latches a phantom signal that a live
     * task later consumes in place of its real wake — the docs warn an
     * overwritten signal "can destroy the internal state" of primitives
     * built on it. Guest enabled bit (0x30) is the liveness authority. */
    {
        const u8* obj = (const u8*)object;
        if (!(obj[0x30 + id / 8] & (u8)(0x80u >> (id & 7)))) {
            /* 2026-08-05 (boot-3 frozen-ticket triage): the gate shipped
             * with NO witness -- a dropped signal to an exited-but-about-
             * to-be-recreated id is exactly the lost-wake shape of the
             * cellSync ticket freeze. Loud counter, plus kill-switch
             * YZ_NO_SENDSIG_SRCH=1 -> restore the pre-gate latch-always
             * behavior for a no-rebuild A/B. */
            static int no_gate = -1;
            static unsigned long nsrch = 0;
            if (no_gate < 0) {
                const char* e = getenv("YZ_NO_SENDSIG_SRCH");
                no_gate = (e && *e == '1') ? 1 : 0;
            }
            nsrch++;
            if (nsrch <= 8 || (nsrch & 1023u) == 0) {
                fprintf(stderr, "[spurs-srch] SendSignal to disabled task "
                        "id=%u taskset=0x%08X n=%lu -> %s\n",
                        id, guest_ea(object), nsrch,
                        no_gate ? "LATCHED (gate off)" : "SRCH (dropped)");
                fflush(stderr);
            }
            if (!no_gate) {
                yz_frontier_trace_emit(YZ_FT_TASK_SIGNAL, ts->wid, id,
                                       guest_ea(object),
                                       ts->tasks[id].waiting ? 1u : 0u,
                                       1u /* dropped: SRCH */, 0, 0, 0);
                mx_unlock(&ts->sync.mutex);
                return CELL_SPURS_TASK_ERROR_SRCH;
            }
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
    yz_frontier_trace_emit(YZ_FT_TASK_SIGNAL, ts->wid, id,
                           guest_ea(object),
                           ts->tasks[id].waiting ? 1u : 0u,
                           0u /* delivered */,
                           (u32)g_yz_sendsig_origin,
#if defined(_WIN32)
                           (u32)GetCurrentThreadId(),
#else
                           0u,
#endif
                           0);
    if (ts->tasks[id].image.image_id == 4 &&
        g_yz_fe0_timeline_enabled) {
        u32 cause = 0u;
        u32 epoch = 0u;
        const int active = yz_fe0_timeline_callback_snapshot(
            &cause, &epoch);
        /* Signals outside a user callback remain visible but uncorrelated. */
        ts->tasks[id].fe0_timeline_cause =
            active ? cause : 0u;
        ts->tasks[id].fe0_timeline_epoch =
            active ? epoch : 0u;
        yz_fe0_timeline_emit(
            YZ_FE0_EVENT_WKL4_SIGNAL,
            ts->tasks[id].fe0_timeline_cause, id,
            ts->tasks[id].fe0_timeline_epoch, ts->wid,
            guest_ea(object), (u32)g_yz_sendsig_origin);
    }
    ts->tasks[id].signalled = 1;
    ts->tasks[id].signal_origin = (u32)g_yz_sendsig_origin;
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
        /* Task-Reference p.38: "still running" is AGAIN; BUSY means another
         * thread is already waiting. A caller polling TryJoinTask2 on AGAIN
         * never terminated against our old BUSY (2026-08-04 audit). */
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
/* Blocking form (imported alongside TryGet; was missing entirely). The
 * writer is a task-completion on another host thread (task_run) — a
 * yield-poll cannot park permanently, matching the file's bounded-wait
 * convention for guest-memory predicates. */
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

/* ------------------------------------------------------------------------- */
/* Event flags                                                               */

static SyncKey g_event_flags[MAX_EVENT_FLAGS];
/* Exact-write publication snapshot: event bits, SPU pending waiter bits,
 * and the PPU pending-receive byte.  Pending must be part of the snapshot:
 * CLEAR_AUTO setters can publish a task wake while leaving the event bits
 * unchanged. */
static u64 g_event_watch_state[MAX_EVENT_FLAGS];
static void queue_notify_guest_write(u32 ea, u32 size, const u64 mask[2]);
static void jobchain_notify_guest_write(u32 ea, u32 size, int ppu_store,
                                        u32 command_mask, u32 bridge_mask);
static void jobguard_notify_guest_write(u32 ea, u32 size, u64 mask);
static TasksetState* event_waiter_taskset(const CellSpursEventFlag* ef,
                                          u32 slot);

static u64 event_watch_snapshot(const u8* object)
{
    return (u64)rd16(object) |
           ((u64)rd16(object + 0x02) << 16) |
           ((u64)object[0x07] << 32);
}

#define EVENT_WATCH_EVENTS      0x000000000000ffffull
#define EVENT_WATCH_SPU_PENDING 0x00000000ffff0000ull
#define EVENT_WATCH_PPU_PENDING 0x000000ff00000000ull
#define EVENT_WATCH_ALL (EVENT_WATCH_EVENTS | EVENT_WATCH_SPU_PENDING | \
                         EVENT_WATCH_PPU_PENDING)

static void event_watch_refresh_fields_locked(SyncKey* sync, u64 fields)
{
    const u32 index = (u32)(sync - g_event_flags);
    const u64 current = event_watch_snapshot((const u8*)sync->key);
    g_event_watch_state[index] =
        (g_event_watch_state[index] & ~fields) | (current & fields);
}

static u32 guest_write_take_bit64(u64* mask)
{
    u32 bit = 0;
    u64 value = *mask;
    while (!(value & 1u)) {
        value >>= 1;
        ++bit;
    }
    *mask &= *mask - 1u;
    return bit;
}

static void taskset_notify_guest_write(u32 ea, u32 size, u64 mask)
{
    const u64 first = ea;
    const u64 last = first + size;
    while (mask) {
        const u32 i = guest_write_take_bit64(&mask);
        TasksetState* ts = &g_tasksets[i];
        if (!ts->sync.live || !ts->sync.key) continue;
        const u32 object_ea = guest_ea(ts->sync.key);
        const u64 signal_first = (u64)object_ea + 0x40u;
        const u64 signal_last = signal_first + 0x10u;
        if (first >= signal_last || last <= signal_first) continue;
        const u32 byte_first = first <= signal_first ? 0u :
            (u32)(first - signal_first);
        const u32 byte_last = last >= signal_last ? 0x10u :
            (u32)(last - signal_first);
        int wake = 0;
        mx_lock(&ts->sync.mutex);
        const u8* object = (const u8*)ts->sync.key;
        for (u32 byte = byte_first; byte < byte_last; ++byte) {
            const u8 signals = object[0x40u + byte];
            for (u32 lane = 0; lane < 8u; ++lane) {
                if (!(signals & (0x80u >> lane))) continue;
                const u32 task_id = byte * 8u + lane;
                TaskState* task = &ts->tasks[task_id];
                if (!task->thread_valid || task->complete || task->signalled)
                    continue;
                if (image4_intercept_signal_locked(ts, task_id)) {
                    /* The guest latch was already published before this
                     * observer ran.  Native completion consumes the same
                     * depth-one signal, so clear its guest bit exactly as the
                     * task-start path would; never leave a replayable wake. */
                    task_bit(ts->sync.key, 0x40, task_id, 0);
                    continue;
                }
                task->signalled = 1;
                task->signal_origin = 5u;
                wake = 1;
            }
        }
        if (wake) cv_wake_all(&ts->sync.cond);
        mx_unlock(&ts->sync.mutex);
    }
}

static void event_notify_guest_write(u32 ea, u32 size, const u64 routed[2])
{
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 half = 0; half < 2; ++half) {
        u64 mask = routed[half];
        while (mask) {
            const u32 i = half * 64u + guest_write_take_bit64(&mask);
            SyncKey* sync = &g_event_flags[i];
            if (!sync->live || !sync->key) continue;
            const u32 object_ea = guest_ea(sync->key);
            if (first >= (u64)object_ea + 8u || last <= object_ea) continue;
            TasksetState* wake_tasksets[16] = {0};
            u8 wake_task_ids[16] = {0};
            u32 wake_count = 0;
            mx_lock(&sync->mutex);
            const u8* object = (const u8*)sync->key;
            const u64 previous = g_event_watch_state[i];
            const u64 state = event_watch_snapshot(object);
            u16 new_pending = 0;
            if (state != previous) {
                /* A lifted SPU EventFlagSet publishes completed task slots
                 * by setting pending bits at +0x02.  The old bridge woke the
                 * PPU event condvar but never delivered the corresponding
                 * depth-one task signal, leaving image-4 blocked until the
                 * broad user-command replay happened to signal every task.
                 * Only newly-pending exact slot edges are dispatched. */
                const u16 pending = (u16)(state >> 16);
                const u16 previous_pending = (u16)(previous >> 16);
                const u16 used = rd16(object + 0x08);
                new_pending =
                    (u16)(pending & (u16)~previous_pending & used);
                g_event_watch_state[i] = state;
                cv_wake_all(&sync->cond);
                for (u32 slot = 0; slot < 16; ++slot) {
                    if (!(new_pending & (0x8000u >> slot))) continue;
                    TasksetState* taskset = event_waiter_taskset(
                        (const CellSpursEventFlag*)object, slot);
                    if (!taskset) continue;
                    wake_tasksets[wake_count] = taskset;
                    wake_task_ids[wake_count] = object[0x50 + slot];
                    ++wake_count;
                }
            }
            mx_unlock(&sync->mutex);
            yz_fe0_timeline_emit(
                YZ_FE0_EVENT_WKL4_EVENT_WRITE, 0u, object_ea,
                (u32)previous, (u32)state,
                ((u32)new_pending << 16) | wake_count, size);
            /* Never nest a taskset mutex below an event-flag mutex.  The
             * guest publication is already complete before the VM-write
             * hook reaches this point, and SendSignal is the SDK's
             * lost-wake-safe depth-one latch. */
            g_yz_sendsig_origin = 4;
            for (u32 wake = 0; wake < wake_count; ++wake)
                _cellSpursSendSignal(
                    (CellSpursTaskset*)wake_tasksets[wake]->sync.key,
                    wake_task_ids[wake]);
            g_yz_sendsig_origin = 0;
        }
    }
}

static _Atomic(uintptr_t) g_guest_write_observer;

static void cellSpursNotifyGuestWriteSource(u32 ea, u32 size, int ppu_store)
{
    const uintptr_t observer_bits = atomic_load_explicit(
        &g_guest_write_observer, memory_order_acquire);
    if (observer_bits)
        ((CellSpursGuestWriteObserver)observer_bits)(ea, size);
    if (g_registry_ready != 2 || !vm_base || !size) return;
    GuestWriteTargets targets;
    if (!guest_write_route_lookup(ea, size, &targets)) return;
    if (targets.tasksets)
        taskset_notify_guest_write(ea, size, targets.tasksets);
    if (targets.events[0] || targets.events[1])
        event_notify_guest_write(ea, size, targets.events);
    if (targets.queues[0] || targets.queues[1])
        queue_notify_guest_write(ea, size, targets.queues);
    if (targets.jobguards)
        jobguard_notify_guest_write(ea, size, targets.jobguards);
    if (targets.jobchain_commands || targets.jobchain_bridges)
        jobchain_notify_guest_write(
            ea, size, ppu_store, targets.jobchain_commands,
            targets.jobchain_bridges);
}

void cellSpursSetGuestWriteObserver(CellSpursGuestWriteObserver observer)
{
    atomic_store_explicit(&g_guest_write_observer, (uintptr_t)observer,
                          memory_order_release);
}

void cellSpursNotifyGuestWrite(u32 ea, u32 size)
{
    cellSpursNotifyGuestWriteSource(ea, size, 0);
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

static void event_observe_native_wait(TasksetState* ts, TaskState* task,
                                      u32 candidate_ea)
{
    if (!ts || !task || (candidate_ea & 127u) ||
        !job_guest_range_valid(candidate_ea, 128u)) {
        yz_fe0_timeline_emit(
            YZ_FE0_EVENT_WKL4_EVENT_OBSERVE,
            task ? task->fe0_timeline_cause : 0u,
            task ? task->id : 0xffffffffu,
            candidate_ea, 0u, 0u, 1u);
        return;
    }

    CellSpursEventFlag* ef =
        (CellSpursEventFlag*)(vm_base + candidate_ea);
    if (ef->bytes[0x0e] > CELL_SPURS_EVENT_FLAG_ANY2ANY ||
        ef->bytes[0x0f] > CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL) {
        yz_fe0_timeline_emit(
            YZ_FE0_EVENT_WKL4_EVENT_OBSERVE,
            task->fe0_timeline_cause, task->id,
            candidate_ea, (u32)event_watch_snapshot(ef->bytes),
            (u32)rd16(ef->bytes + 0x08),
            2u | ((u32)ef->bytes[0x0d] << 8) |
                ((u32)ef->bytes[0x0e] << 16) |
                ((u32)ef->bytes[0x0f] << 24));
        return;
    }

    const u16 used = rd16(ef->bytes + 0x08);
    int matched_waiter = 0;
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(used & (0x8000u >> slot)) ||
            ef->bytes[0x50 + slot] != task->id)
            continue;
        if (event_waiter_taskset(ef, slot) == ts) {
            matched_waiter = 1;
            break;
        }
    }
    if (!matched_waiter) {
        yz_fe0_timeline_emit(
            YZ_FE0_EVENT_WKL4_EVENT_OBSERVE,
            task->fe0_timeline_cause, task->id,
            candidate_ea, (u32)event_watch_snapshot(ef->bytes),
            (u32)used,
            3u | ((u32)ef->bytes[0x0d] << 8) |
                ((u32)ef->bytes[0x0e] << 16) |
                ((u32)ef->bytes[0x0f] << 24));
        return;
    }

    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 1);
    if (!sync) {
        yz_fe0_timeline_emit(
            YZ_FE0_EVENT_WKL4_EVENT_OBSERVE,
            task->fe0_timeline_cause, task->id,
            candidate_ea, (u32)event_watch_snapshot(ef->bytes),
            (u32)used, 4u);
        return;
    }
    const u32 index = (u32)(sync - g_event_flags);
    TasksetState* wake_tasksets[16] = {0};
    u8 wake_task_ids[16] = {0};
    u32 wake_count = 0;

    mx_lock(&sync->mutex);
    if (!guest_write_route_target_generation(
            GUEST_WRITE_ROUTE_EVENT, index)) {
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_EVENT, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_EVENT, index,
                                candidate_ea, 8u);
        /* Registration races a setter.  Treat every already-pending slot as
         * an undelivered edge, then snapshot the complete state below. */
        g_event_watch_state[index] =
            event_watch_snapshot(ef->bytes) & ~EVENT_WATCH_SPU_PENDING;
    }

    const u64 previous = g_event_watch_state[index];
    const u64 state = event_watch_snapshot(ef->bytes);
    const u16 pending = (u16)(state >> 16);
    const u16 previous_pending = (u16)(previous >> 16);
    const u16 new_pending =
        (u16)(pending & (u16)~previous_pending & rd16(ef->bytes + 0x08));
    if (state != previous) {
        g_event_watch_state[index] = state;
        cv_wake_all(&sync->cond);
    }
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(new_pending & (0x8000u >> slot))) continue;
        TasksetState* waiter = event_waiter_taskset(ef, slot);
        if (!waiter) continue;
        wake_tasksets[wake_count] = waiter;
        wake_task_ids[wake_count] = ef->bytes[0x50 + slot];
        ++wake_count;
    }
    mx_unlock(&sync->mutex);

    yz_fe0_timeline_emit(
        YZ_FE0_EVENT_WKL4_EVENT_OBSERVE,
        task->fe0_timeline_cause, task->id,
        candidate_ea, (u32)state,
        ((u32)new_pending << 16) | wake_count,
        5u | (index << 8));

    g_yz_sendsig_origin = 4;
    for (u32 wake = 0; wake < wake_count; ++wake)
        _cellSpursSendSignal(
            (CellSpursTaskset*)wake_tasksets[wake]->sync.key,
            wake_task_ids[wake]);
    g_yz_sendsig_origin = 0;
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
    SyncKey* sync = sync_get(g_event_flags, MAX_EVENT_FLAGS, ef, 1);
    if (!sync) return CELL_SPURS_TASK_ERROR_NOMEM;
    const u32 index = (u32)(sync - g_event_flags);
    guest_write_route_begin_target(GUEST_WRITE_ROUTE_EVENT, index);
    guest_write_route_watch(GUEST_WRITE_ROUTE_EVENT, index,
                            guest_ea(ef), 8u);
    g_event_watch_state[index] = event_watch_snapshot(ef->bytes);
    return CELL_OK;
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
extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
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
    const u32 event_index = (u32)(sync - g_event_flags);
    mx_lock(&sync->mutex);
    /* 2026-08-06 (boot 50 flight recorder): SERIALIZE this whole RMW section
     * against SPU lock-line commits, exactly as the queue paths do (line
     * ~2061). The waiting tasks' pending-bit clears (+0x02) and re-arms are
     * committed SPU-side as 128-byte PUTLLC line commits; this section's
     * rd16(+0x02) ... wr16(+0x02, old|pending) window otherwise RESURRECTS
     * every bit cleared in between — one racy Set excludes all five wid-4
     * waiter slots from `used` FOREVER (measured terminal state: 2,800
     * signals to the CRI taskset, zero to wid-4, pack parked, no banner).
     * Same defect class as the cellSync mutex RMW fix (08-05). HARDENING
     * until the A/B: kill-switch YZ_EF_NO_LOCKLINE=1 restores the old
     * unserialized behavior. */
    static int ef_no_lockline = -1;
    if (ef_no_lockline < 0) {
        const char* e = getenv("YZ_EF_NO_LOCKLINE");
        ef_no_lockline = (e && *e == '1') ? 1 : 0;
    }
    if (!ef_no_lockline) spu_lockline_lock();
    u16 events = (u16)(rd16(ef->bytes) | bits);
    const u16 wtn_pend02_before = rd16(ef->bytes + 0x02);
    const u16 wtn_used08 = rd16(ef->bytes + 0x08);
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
    /* Q2-A (contract audit; rpcs3 cellSpurs.cpp:3354-3370): a PPU waiter
     * registers via ppuWaitMask[0x04] + ppuWaitSlotAndMode[0x06], NOT the
     * task slot table walked above. Deliver to it explicitly: latch the got
     * bits into its receive slot, raise ppuPendingRecv[0x07], clear the
     * mask, and count the bits consumed so CLEAR_AUTO cannot lose the
     * delivery to a racing task-slot consumer inside the waiter's poll gap. */
    {
        const u16 ppu_mask = rd16(ef->bytes + 0x04);
        if (ppu_mask && !ef->bytes[0x07]) {
            const u32 ppu_slot = ef->bytes[0x06] >> 4;
            const int ppu_mode = ef->bytes[0x06] & 0x0F;
            if (event_ready(events, ppu_mask, ppu_mode)) {
                const u16 got = (u16)(events & ppu_mask);
                wr16(ef->bytes + 0x30 + ppu_slot * 2, got);
                ef->bytes[0x07] = 1;
                wr16(ef->bytes + 0x04, 0);
                consumed |= got;
            }
        }
    }
    if (ef->bytes[0x0f] == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) events &= (u16)~consumed;
    wr16(ef->bytes, events);
    /* Deliver every pending edge not yet represented by the exact-write
     * bridge, including a lifted-SPU publication that completed just before
     * this HLE setter acquired the lock line.  This closes the
     * writer-before-notifier race without ever replaying an old pending bit. */
    const u16 routed_pending = (u16)(
        rd16(ef->bytes + 0x02) &
        (u16)~(u16)(g_event_watch_state[event_index] >> 16) &
        rd16(ef->bytes + 0x08));
    for (u32 slot = 0; slot < 16; ++slot) {
        if (!(routed_pending & (0x8000u >> slot))) continue;
        TasksetState* taskset = event_waiter_taskset(ef, slot);
        if (taskset) {
            wake_tasksets[wake_count] = taskset;
            wake_task_ids[wake_count] = ef->bytes[0x50 + slot];
            ++wake_count;
        }
    }
    /* Snapshot while the lock line still belongs to this setter.  Refreshing
     * after unlock could absorb a newer lifted-SPU edge before its notifier
     * ran, falsely marking that wake as delivered. */
    event_watch_refresh_fields_locked(sync, EVENT_WATCH_ALL);
    if (!ef_no_lockline) spu_lockline_unlock();
    /* Set-path witness (flight recorder): before/after slot state + wake
     * fanout. The boot-50 exclusion face reads as pend02-before holding the
     * victim slots' bits with used==0 for them and wake_count collapsed. */
    yz_frontier_trace_emit(YZ_FT_EVENT_SET, 0 /* tid: adjacent evq records
                           carry the caller */,
                           guest_ea(ef),
                           ((u32)bits << 16) | events,
                           ((u32)wtn_used08 << 16) | wtn_pend02_before,
                           ((u32)used << 16) | pending,
                           wake_count,
                           wake_count ? guest_ea(wake_tasksets[0]->sync.key) : 0u,
                           ((u32)ef->bytes[0x0e] << 8) | ef->bytes[0x0f]);
    {
        static _Atomic long efset_n = 0;
        long en = atomic_fetch_add(&efset_n, 1) + 1;
        if (en <= 4)
            fprintf(stderr, "[ef-set] ea=0x%08X bits=0x%04X used08=0x%04X "
                    "pend02=0x%04X wake=%u n=%ld\n",
                    guest_ea(ef), bits, wtn_used08, wtn_pend02_before,
                    wake_count, en);
    }
    cv_wake_all(&sync->cond);
    mx_unlock(&sync->mutex);
    /* Do not nest a taskset mutex under the event-flag mutex.  Task code can
     * publish event-flag state while its scheduler owns the taskset mutex. */
    g_yz_sendsig_origin = 1;
    for (u32 i = 0; i < wake_count; ++i)
        _cellSpursSendSignal(
            (CellSpursTaskset*)wake_tasksets[i]->sync.key, wake_task_ids[i]);
    g_yz_sendsig_origin = 0;
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
        event_watch_refresh_fields_locked(sync, EVENT_WATCH_EVENTS);
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
        /* Q2-B (contract audit vs rpcs3 cellSpurs.cpp:3552-3567): the PPU
         * waiter records its slot ONLY in ppuWaitSlotAndMode[0x06]; it must
         * NOT set a bit in spuTaskUsedWaitSlots[0x08]. A phantom used bit
         * with a zero spuTaskWaitMask[0x10+slot*2] makes the SPU-side
         * firmware Set treat the slot as an always-satisfied task waiter
         * and _cellSpursSendSignal task id 0 on every Set — spurious wakes
         * for an unrelated task, and our own Set's used&~pending scan
         * misroutes deliveries. */
    }
    ef->bytes[0x06] = (u8)((slot << 4) | mode);
    ef->bytes[0x07] = 0;
    wr16(ef->bytes + 0x04, mask);
    atomic_thread_fence(memory_order_release);

    while (!ef->bytes[0x07] && !event_ready(rd16(ef->bytes), mask, mode))
        /* Timed: lifted-SPU tasks set flag bytes via guest memory/MFC
         * atomics without waking this HLE condvar (cv_wait_ms). */
        cv_wait_ms(&sync->cond, &sync->mutex, 2u);

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
    /* Q2-B: no [0x08] bit was set on entry, so none is cleared here. */
    /* S4: a Set racing our raw-bits exit may have read ppuWaitMask before
     * we zeroed it and completed delivery after — leaving ppuPendingRecv=1
     * with nobody waiting, which would turn EVERY later Wait into BUSY at
     * the entry gate (permanent). We are the only PPU waiter; any pending
     * delivery was ours, so consume the marker on the way out. */
    ef->bytes[0x07] = 0;
    event_watch_refresh_fields_locked(
        sync, EVENT_WATCH_EVENTS | EVENT_WATCH_PPU_PENDING);
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
    event_watch_refresh_fields_locked(sync, EVENT_WATCH_EVENTS);
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
    u64 watch_state[2];
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
        /* S2(i): the signal is LATCHED — never gate on task->waiting. The
         * SPU consumer publishes its wait token BEFORE entering SPURS
         * WAITING; a push in that window used to find waiting==0, skip the
         * slot, and drop the wake permanently (textbook lost wakeup). */
        if (task->thread_valid && !task->complete) {
            task->signalled = 1;
            task->signal_origin = 6u;
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

static void queue_notify_guest_write(u32 ea, u32 size, const u64 routed[2])
{
    const u64 first = ea;
    const u64 last = first + size;
    for (u32 half = 0; half < 2; ++half) {
        u64 mask = routed[half];
        while (mask) {
            const u32 i = half * 64u + guest_write_take_bit64(&mask);
            QueueState* q = &g_queues[i];
            if (!q->sync.live || !q->sync.key) continue;
            const u64 object_ea = guest_ea(q->sync.key);
            if (first >= object_ea + 16u || last <= object_ea) continue;
            mx_lock(&q->sync.mutex);
            const u8* object = (const u8*)q->sync.key;
            const u64 state0 = rd64(object + 0x00);
            const u64 state1 = rd64(object + 0x08);
            if (state0 != q->watch_state[0] ||
                state1 != q->watch_state[1]) {
                const u64 previous0 = q->watch_state[0];
                const u64 previous1 = q->watch_state[1];
                q->watch_state[0] = state0;
                q->watch_state[1] = state1;
                cv_wake_all(&q->sync.cond);
            }
            mx_unlock(&q->sync.mutex);
        }
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
    q->watch_state[0] = rd64((u8*)object + 0x00);
    q->watch_state[1] = rd64((u8*)object + 0x08);
    {
        const u32 index = (u32)(q - g_queues);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_QUEUE, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_QUEUE, index,
                                guest_ea(object), 16u);
    }
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
    q->watch_state[0] = rd64((u8*)object + 0x00);
    q->watch_state[1] = rd64((u8*)object + 0x08);
    {
        const u32 index = (u32)(q - g_queues);
        guest_write_route_begin_target(GUEST_WRITE_ROUTE_QUEUE, index);
        guest_write_route_watch(GUEST_WRITE_ROUTE_QUEUE, index,
                                guest_ea(object), 16u);
    }
    if (g_yz_fe0_timeline_enabled) {
        yz_fe0_timeline_emit(
            YZ_FE0_EVENT_WKL4_QUEUE_INIT, 0u,
            (u32)(q - g_queues), guest_ea(object), guest_ea(owner),
            (size << 16) | depth, (direction << 16) | q->kind);
    }
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
    s32 raw_head = (s32)rd32((const u8*)object + 0x00);
    s32 raw_tail = (s32)rd32((const u8*)object + 0x04);
    const u32 period = q->depth * 2;
    /* Q1-C (firmware oracle, libsre @0x02016A8C/0x02016B78): a NEGATIVE
     * pointer word is the one's-complement "peer parked here" encoding, not
     * a transient reservation — the real value is ~v. Treating it as
     * retry-later made every push/pop spin forever once an SPU-side peer
     * parked (the GET==PUT presents-frozen wedge shape). */
    if (raw_head < 0) raw_head = (s32)~(u32)raw_head;
    if (raw_tail < 0) raw_tail = (s32)~(u32)raw_tail;
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
            /* Firmware contract (libsre cellSpursQueuePushBody @0x02016BF8-
             * 0x02016C90): on the empty->non-empty transition, a consumer
             * task that parked itself in the queue object (has-waiter flag
             * at +0x20, task id at +0x21, workload id at +0x41, taskset EA
             * at +0x60) receives _cellSpursSendSignal. The lifted SPU-side
             * queue routine writes that record before entering WAIT_SIGNAL;
             * without the signal the consumer sleeps forever while the
             * producer's memory looks perfectly intact (the Akiyama/Hana
             * effect-stream null-object crash). */
            {
                const u8* obj = (const u8*)object;
                if (count == 0 && obj[0x20]) {
                    const u64 ts_ea = rd64(obj + 0x60);
                    if (ts_ea && vm_base) {
                        g_yz_sendsig_origin = 2;
                        _cellSpursSendSignal(
                            (CellSpursTaskset*)(vm_base + (u32)ts_ea),
                            obj[0x21]);
                        g_yz_sendsig_origin = 0;
                        static _Atomic long wake_n = 0;
                        long wn = atomic_fetch_add(&wake_n, 1) + 1;
                        if (wn <= 8 || (wn & (wn - 1)) == 0)
                            diag("queue-push-wake-consumer", object,
                                 ((u64)(u32)wn << 32) | obj[0x21]);
                    } else {
                        static _Atomic long nots_n = 0;
                        if (atomic_fetch_add(&nots_n, 1) < 8)
                            diag("queue-push-waiter-no-taskset", object,
                                 obj[0x41]);
                    }
                }
            }
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        /* Timed: the peer side may be lifted SPU code mutating the guest
         * object without an HLE wake (cv_wait_ms). */
        cv_wait_ms(&q->sync.cond, &q->sync.mutex, 2u);
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
            /* Producer wake on the full->not-full edge. S1 (firmware oracle
             * cellSpursQueuePopBody @0x02016F40/0x02016F58/0x02016F5C): the
             * PRODUCER waiter record lives at bytes 0x30/0x31/0x51 — a
             * separate record from the consumer's 0x20/0x21/0x41 the push
             * side reads. Spurious signals are safe (latched; predicates
             * re-checked). */
            if (!peek) {
                const u8* obj = (const u8*)object;
                if (count == q->depth && obj[0x30]) {
                    const u64 ts_ea = rd64(obj + 0x60);
                    if (ts_ea && vm_base) {
                        g_yz_sendsig_origin = 3;
                        _cellSpursSendSignal(
                            (CellSpursTaskset*)(vm_base + (u32)ts_ea),
                            obj[0x31]);
                        g_yz_sendsig_origin = 0;
                        static _Atomic long pwake_n = 0;
                        long pn = atomic_fetch_add(&pwake_n, 1) + 1;
                        if (pn <= 8 || (pn & (pn - 1)) == 0)
                            diag("queue-pop-wake-producer", object,
                                 ((u64)(u32)pn << 32) | obj[0x31]);
                    }
                }
            }
            cv_wake_all(&q->sync.cond);
            mx_unlock(&q->sync.mutex);
            return CELL_OK;
        }
        spu_lockline_unlock();
        if (!blocking) {
            mx_unlock(&q->sync.mutex);
            return CELL_SPURS_TASK_ERROR_AGAIN;
        }
        /* Timed: the peer side may be lifted SPU code mutating the guest
         * object without an HLE wake (cv_wait_ms). */
        cv_wait_ms(&q->sync.cond, &q->sync.mutex, 2u);
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
        /* Timed: the peer side may be lifted SPU code mutating the guest
         * object without an HLE wake (cv_wait_ms). */
        cv_wait_ms(&q->sync.cond, &q->sync.mutex, 2u);
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
/* 2026-08-05 (boots 35/36): the dialogue-era command list spans ~700 slots;
 * at the old cap of 512 the tables saturated SILENTLY, jobchain_command_slot
 * returned -1 for new slots, publication/consumed tracking went blind, and
 * the scan eventually claimed an unpublished region and died on
 * INVALID_BIN (boot 36, error 0x80410A1F, image-miss fp=0x3A591273...). */
#define MAX_JOB_PARKED_SLOTS 4096
#define MAX_JOB_LEDGER_ENTRIES 128
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
    /* Pending-ticket bridge window: dispatched descriptors' kind words.
     * SEPARATE from watch_lo/hi — routing ring stores through the command
     * machinery saturated the parked/command tables (boot 35, parked=512)
     * and degraded publication tracking. */
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
    /* The global store-notify window must span BOTH windows. */
    if (jc->bridge_lo < g_native_spurs_ppu_watch_lo)
        g_native_spurs_ppu_watch_lo = jc->bridge_lo;
    if (jc->bridge_hi > g_native_spurs_ppu_watch_hi)
        g_native_spurs_ppu_watch_hi = jc->bridge_hi;
}

static void jobchain_ledger_record(JobChainState* jc, u32 type, u32 ea,
                                   u32 aux, u64 value)
{
    /* Allocation-free flight record of submission/claim/park/publication and
     * redispatch edges.  Keep this independent of the verbose ledger flag so
     * a diagnostic build retains the transition tail without stderr I/O. */
    /* J/D already have the richer YZ_FT_JOB start/done pair. Avoid doubling
     * the hottest event and retain well over the hard-confirmation window. */
    if (type != 'J' && type != 'D')
        yz_frontier_trace_emit(
            YZ_FT_JOBCHAIN, jc->wid, guest_ea(jc->sync.key),
            type, ea, aux, (u32)(value >> 32), (u32)value,
            (jc->waiting_command ? 1u : 0u) |
                (jc->running ? 2u : 0u) | (jc->complete ? 4u : 0u));
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
                    if (attempt) {
                        fprintf(stderr,
                                "[jc-joblist] acquired published list=%08X "
                                "after %u retries count=%u size=0x%X "
                                "first=%08X\n",
                                ea, attempt, count, descriptor_size,
                                (u32)first);
                        fflush(stderr);
                    }
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
                        "ea=%08X — publication tracking degraded\n",
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
    /* 2026-08-05 (audit H5): bound the stable-snapshot "grab window" like
     * hardware. The SPU fetches the command list in 128-byte units and
     * grabs at most maxGrabbedJob jobs per grab; our old capture walked up
     * to 512 commands across the whole chain, widening the stale-view
     * window ~100x into the producer's legally-mutating region. Sequential
     * advancement past the current 128-byte line ends the SNAPSHOT (the
     * executor falls back to live reads / a fresh capture there); flow
     * control (NEXT/CALL/RET) models a new fetch and re-bases the line.
     * The job budget spans the whole capture.
     * grab_bounded=0 is the PUBLICATION-time snapshot (the producer's
     * published prefix, bounded by the barrier set, not by the grab
     * window -- a published stable view must survive producer slot reuse
     * regardless of maxGrabbedJob; the claimed-publication regression
     * test covers this). grab_bounded=1 is the claim-time fetch. */
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
         * in an immutable generation.  Acceptance run 6 exposed why this is
         * essential for CALL: a snapshot followed the published CALL into a
         * dynamic sub-list before its tail RET appeared, preserved that zero,
         * and then walked through unrelated guest data until it decoded an
         * accidental END.  The completing store must instead remain visible
         * to the live executor. */
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
                 * completed it.  Acceptance run 19 captured an old zero copy
                 * of 0x401B7F80, then failed INVALID_BIN even though the live
                 * descriptor was complete by the time execution arrived. */
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
    /* grab_bounded=0: this claim consumes a PUBLISHED view (parked-slot
     * resume). Bounding it by maxGrabbedJob breaks the publication-claim
     * protocol -- the producer legally reuses consumed slots immediately
     * after notify, trusting the claim took the whole published prefix
     * (MEASURED 2026-08-05: tests/native_spurs line ~940 fails 10/10 with
     * the bound here). Audit H5's literal fix is incompatible with this
     * architecture; a future bound must clip by PUBLISHED EXTENT instead. */
    const u32 count = jobchain_capture_commands(
        jc, start_ea, jc->command_snapshot, /*grab_bounded=*/0);
    jc->command_snapshot_count = count;
    jc->command_snapshot_index = 0;
    jobchain_ledger_record(jc, 'C', start_ea, count,
                           count ? jc->command_snapshot[0].command : 0);
    return count;
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
    jobchain_ledger_record(jc, 'C', slot_ea, 1, descriptor_ea);
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
                    "ea=%08X — wrapped publications unwatched\n",
                    (unsigned)MAX_JOB_PARKED_SLOTS, ea);
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

static u32 jobchain_queue_snapshot(JobChainState* jc, u32 start_ea,
                                   int replace_unconsumed)
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
        jc, start_ea, snapshot->command, /*grab_bounded=*/0);
    if (!snapshot->count) {
        free(snapshot);
        jobchain_ledger_record(jc, 'Q', start_ea, 0, 0);
        return 0;
    }
    snapshot->sequence = ++jc->pending_snapshot_sequence;
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
            jobchain_ledger_record(jc, 'U', start_ea, snapshot->count,
                                   snapshot->command[0].command);
            return snapshot->count;
        }
    }
    if (jc->pending_snapshot_tail)
        jc->pending_snapshot_tail->next = snapshot;
    else
        jc->pending_snapshot_head = snapshot;
    jc->pending_snapshot_tail = snapshot;
    jobchain_ledger_record(jc, 'Q', start_ea, snapshot->count,
                           snapshot->command[0].command);
    return snapshot->count;
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
            jobchain_ledger_record(jc, 'V', start_ea,
                                   (u32)queued_sequence, 0);
            free(snapshot);
        } else {
            previous = snapshot;
        }
        snapshot = next;
    }
}

static u32 jobchain_queue_persistent_job(
    JobChainState* jc, u32 slot_ea, u32 descriptor_ea,
    const u8* acquired_descriptor)
{
    PendingJobSnapshot* snapshot =
        (PendingJobSnapshot*)calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        jobchain_ledger_record(jc, 'F', slot_ea, 0, descriptor_ea);
        return 0;
    }
    snapshot->start_ea = slot_ea;
    snapshot->count = 1;
    snapshot->sequence = ++jc->pending_snapshot_sequence;
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
    jobchain_ledger_record(jc, 'Q', slot_ea, 1, descriptor_ea);
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

/* The title's synchronous producer exposes descriptor+0x10 = 6 and a nonzero
 * size_io before it finishes the descriptor body, executes hwsync, and waits
 * for the persistent dispatcher JOB to clear the ticket. A raw store edge is
 * therefore only a candidate. Lifted PPU writes become eligible at the same
 * producer thread's next guest fence; completed SPU DMA/HLE transfers carry
 * their own publication edge. This preserves the hardware orbit without a
 * timing-based poll of partially constructed guest memory. */
enum JobchainBridgeSource {
    JC_BRIDGE_GUEST_PUBLISH = 0,
    JC_BRIDGE_PPU_FENCE = 1
};

static u32 jobchain_resume_pending_ticket_locked(JobChainState* jc,
                                                 u32 descriptor_filter,
                                                 int source)
{
    int selected = 0;
    u32 selected_slot_ea = 0;
    u32 selected_desc = 0;
    u64 selected_command = 0;
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
        if (descriptor_filter && desc == descriptor_filter)
            yz_frontier_trace_emit(
                YZ_FT_COMPLETION, jc->wid, 2u,
                desc, desc + 0x10u, ticket, 6u, ready,
                source == JC_BRIDGE_PPU_FENCE ? 2u : 1u);
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
            selected_command = command;
            selected_sequence = sequence;
            memcpy(selected_descriptor, candidate_descriptor,
                   jc->descriptor_size);
        }
    }
    if (!selected)
        return 0;

    yz_frontier_trace_emit(
        YZ_FT_COMPLETION, jc->wid, 3u,
        selected_desc, selected_desc + 0x10u,
        rd32(vm_base + selected_desc + 0x10u), 6u,
        rd32(vm_base + selected_desc + 0x14u),
        source == JC_BRIDGE_PPU_FENCE ? 2u : 1u);

    jobchain_ledger_record(jc, 'B', selected_slot_ea, selected_desc,
                           selected_command);
    {
        static atomic_uint bridge_n;
        const unsigned k = atomic_fetch_add(&bridge_n, 1u);
        if (k < 24u || (k & (k + 1u)) == 0u) {
            fprintf(stderr,
                    "[jc-bridge] pending ticket desc=%08X kind=6 "
                    "-> redispatch slot=%08X parked=%d source=%s "
                    "(#%u)\n",
                    selected_desc, selected_slot_ea,
                    jc->waiting_command,
                    source == JC_BRIDGE_PPU_FENCE ?
                        "ppu-fence" : "guest-publish",
                    k + 1u);
            fflush(stderr);
        }
    }
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
            jobchain_defer_resume(jc, selected_slot_ea, selected_desc,
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
     * 16-byte lane. Acceptance run 13 exposed descriptor 0x40603BB0; the old
     * 128-byte mask attributed its +0x14 store to unrelated storage. */
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
        /* Cheap LOCK-FREE pre-filter: only a small store landing on a ring
         * descriptor's kind word with the pending value is a bridge
         * candidate. Ordinary ring traffic (whole-slot builds, thousands
         * per second on t1's submit path) must NOT take the chain mutex —
         * boots 37-41 showed the early frame-drain wedge rate exploding
         * under exactly that contention. */
        const int in_bridge_window = (bridge_mask & (1u << i)) != 0;
        u32 bridge_descriptor = 0;
        const int bridge_candidate =
            jobchain_pending_ticket_publish_candidate(
                jc, ea, size, in_bridge_window, &bridge_descriptor);
        /* Record both halves of a completion descriptor's two-store publish.
         * Redispatch becomes eligible only when either write completes the
         * tuple { ticket == 6, ready != 0 }. */
        const u32 descriptor = ea & ~0x7fu;
        const u32 descriptor_offset = ea & 0x7fu;
        const int completion_field_write =
            in_bridge_window && size <= 8u && descriptor_offset < 0x18u &&
            descriptor_offset + size > 0x10u &&
            job_guest_range_valid(descriptor + 0x10u, 8u);
        /* Only retain the pending-ticket protocol under investigation.  Other
         * descriptor families publish these same two words at very high rate
         * and would evict the useful dependency history without adding signal. */
        if (completion_field_write &&
            rd32(vm_base + descriptor + 0x10u) == 6u)
            yz_frontier_trace_emit(
                YZ_FT_COMPLETION, 0u, 1u,
                descriptor, ea,
                rd32(vm_base + descriptor + 0x10u), 0u,
                rd32(vm_base + descriptor + 0x14u),
                bridge_candidate ? 1u : 0u);
        if (!in_command_window && !bridge_candidate) continue;
        mx_lock(&jc->sync.mutex);
        int wake_transition = 0;
        u64 exact_publication_sequence = 0;
        if (in_command_window && size == sizeof(u64) && !(ea & 7u) &&
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
                int slot = jobchain_command_slot(jc, ea, 0);
                if (slot < 0 &&
                    jobchain_plausible_publication_head_locked(jc, ea))
                    slot = jobchain_command_slot(jc, ea, 1);
                if (slot >= 0)
                    exact_publication_sequence =
                        ++jc->command_publication_sequence[slot];
            }
        }
        /* PENDING-TICKET BRIDGE (2026-08-05, dialogue-load wedge, boots
         * 25-33, 7/7). The title's synchronous loads submit a ring ticket
         * by writing kind 6 into descriptor+0x10 (MEASURED: t1 via
         * func_00AA0D50 <- 00454C90 <- 00456BC0) and then SPIN IN PLACE
         * until the SPU side clears it — no jobchain command is ever
         * appended for it (t1 is the appender and it is blocked). On real
         * HW the free-running kernel re-executes the stale JOB command
         * still in the circular list and the self-gating dispatcher (bin
         * 0x01254500) picks the ticket up; our parked scanner never
         * revisits consumed commands, so the ticket starved and t1 hung.
         * Bridge: when a write to either readiness word leaves the complete
         * tuple { kind 6, size_io nonzero }, re-dispatch the command slot that last
         * dispatched that descriptor — the event-driven equivalent of the
         * real kernel's next orbit. A double dispatch is a no-op: the
         * dispatcher reads kind 0 and returns (jobput census, boots
         * 27-30). Kill-switch YZ_NO_JC_BRIDGE=1 for A/B. */
        int bridge_resumed = 0;
        if (!jobchain_bridge_disabled() && bridge_candidate) {
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
                    jc, bridge_descriptor, JC_BRIDGE_GUEST_PUBLISH);
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
            if (native_trace_enabled()) {
                fprintf(stderr,
                        "[native-spurs-trace] jobchain-publish ea=0x%08X "
                        "size=%u head=0x%08X command=%016llX "
                        "claimed=%u queued=%u\n",
                        ea, size, slot_ea,
                        (unsigned long long)command, claimed, queued);
            }
            wake_transition |= claimed != 0;
            wake_transition |= queued != 0 && jc->waiting_command;
        }
        /* The first publication of a circular slot can race ahead of the
         * native worker ever parking there.  Claim a complete synchronization
         * command only at a known command slot or the immediate successor of
         * one.  The broad watch envelope can span distant CALL lists; treating
         * every value 2/10/18/26 inside that gap as a barrier queued unrelated
         * title data during acceptance run 20. */
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
                if (native_trace_enabled()) {
                    fprintf(stderr,
                            "[native-spurs-trace] jobchain-barrier "
                            "ea=0x%08X command=%016llX queued=%u\n",
                            ea, (unsigned long long)command, queued);
                }
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
    if (jobchain_bridge_disabled()) return;
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
                    jc, candidate[p], JC_BRIDGE_PPU_FENCE) != 0;
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

/* Test support (2026-08-05): report whether a jobchain's worker is parked
 * waiting for a command at guest EA `ea`. The claimed-SYNC publication
 * regression test needs this deterministic parked edge before writing its
 * exact SYNC; the brief_sleep it used instead lost the race ~2-3/12 runs
 * (the known pre-existing job_runs flake). Not part of any SDK surface. */
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
    jc->bridge_lo = UINT32_MAX;
    jc->bridge_hi = 0;
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
    {
        const u32 index = (u32)(jc - g_jobchains);
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_JOBCHAIN_COMMAND, index);
        guest_write_route_begin_target(
            GUEST_WRITE_ROUTE_JOBCHAIN_BRIDGE, index);
    }
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

/* Mirror the native runner's LS allocation without writing anything. SPURS
 * jobchains have two standard binary slots; large jobs may fit at only one of
 * them once their input, cache, scratch and stack reservations are included.
 * Blind alternation is therefore not a valid slot-selection rule. */
static int job_layout_fits(const u8* d, u32 bin_size,
                           u32 dma_list_size, u32 cache_list_size, u32 slot)
{
    const u32 descriptor_ls = 0x3f000;
    const u32 size_io = rd32(d + 0x14) & 0x3ffffu;
    const u32 size_out = rd32(d + 0x18) & 0x3ffffu;
    const u32 size_stack = rd16(d + 0x1c) ?
                           (u32)rd16(d + 0x1c) * 16u : 8192u;
    const u32 size_scratch = (u32)rd16(d + 0x1e) * 16u;
    /* The SPU ABI's initial stack consists of a minimal caller frame, a
     * link-register save area and the entry frame's backchain. */
    if (size_stack < 0x30u)
        return 0;
    if (bin_size > descriptor_ls - slot)
        return 0;

    u32 cursor = (slot + bin_size + 1023u) & ~1023u;
    if (size_io && !job_alloc(&cursor, descriptor_ls, size_io, 1024))
        return 0;
    if (size_out && !job_alloc(&cursor, descriptor_ls, size_out, 1024))
        return 0;
    for (u32 i = 0; i < cache_list_size / 8; ++i) {
        const u64 item = rd64(d + 0x30 + dma_list_size + i * 8);
        const u32 item_size = (u32)(item >> 32);
        if (item_size && !job_alloc(
                &cursor, descriptor_ls, item_size, 1024))
            return 0;
    }
    return job_alloc(&cursor, descriptor_ls,
                     size_scratch + size_stack, 1024) != 0;
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
    (void)opaque;
    /* S9: drain the job's outstanding MFC transfers before run_job publishes
     * the output areas and frees the context — the task-side scheduler
     * handoffs do exactly this so a wait cannot expose a partial context;
     * job exit needs the same barrier or a late PUT races publication. */
    spu_task_wait_all_dma(ctx);
    return -1;
}

static int job_io_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_NATIVE_SPURS_JOB_IO_TRACE") ? 1 : 0;
    return enabled;
}

static u32 job_io_trace_binary(void)
{
    static u32 binary;
    static int configured;
    if (!configured) {
        const char* text = getenv("YZ_NATIVE_SPURS_JOB_IO_TRACE_BINARY");
        binary = text && *text ? (u32)strtoul(text, NULL, 0) : 0x01252680u;
        configured = 1;
    }
    return binary;
}

static u32 job_io_trace_min_kind(void)
{
    static u32 kind;
    static int configured;
    if (!configured) {
        const char* text = getenv("YZ_NATIVE_SPURS_JOB_IO_TRACE_MIN_KIND");
        kind = text && *text ? (u32)strtoul(text, NULL, 0) : 0u;
        configured = 1;
    }
    return kind;
}

/* Kill-switch YZ_JOB_INOUT_WB=1 restores the RETIRED automatic inout
 * write-back (the boot-24 measured descriptor corruption) for
 * single-variable A/B. Default: no write-back, per the SPURS contract. */
static int job_inout_wb_forced(void)
{
    static int forced = -1;
    if (forced < 0) {
        const char* e = getenv("YZ_JOB_INOUT_WB");
        forced = (e && *e == '1') ? 1 : 0;
        if (forced) {
            fprintf(stderr, "[jobpub] KILL-SWITCH: legacy inout write-back "
                    "FORCED ON (YZ_JOB_INOUT_WB=1)\n");
            fflush(stderr);
        }
    }
    return forced;
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

/* In-flight job hang watcher (2026-08-05, wedge boots 25-28): at the
 * dialogue load, jobs cease to FLOW while zero executed jobs exit STALE —
 * so the stuck submission either never starts or never RETURNS. Track
 * in-flight jobs; a watcher prints any older than 4 s with its live LS pc
 * (racy read, diagnostic only) — a hung job names its spin site directly. */
#define JOB_INFLIGHT_MAX 16
typedef struct {
    void* volatile ctx;              /* spu_context*; NULL = slot free */
    u32 desc, bin, d10;
    unsigned long long start_ms;
} JobInflightSlot;
static JobInflightSlot g_job_inflight[JOB_INFLIGHT_MAX];
static atomic_int g_job_watch_started;
static atomic_uint g_frontier_job_completion_serial;

u32 yz_frontier_job_completion_serial(void)
{
    return atomic_load_explicit(
        &g_frontier_job_completion_serial, memory_order_relaxed);
}

static unsigned long long job_watch_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* Job-flow stall detector (boots 25-30): the wedge face is "jobs stop being
 * consumed while a live chain waits at a slot the game has already filled".
 * When no job has ENTERED run_job for >6 s while a chain is live+running,
 * dump every chain's state, ledger ring (YZ_NATIVE_SPURS_LEDGER) and the
 * LIVE GUEST command words at its current/parked slots — the direct
 * discriminator between "publication missed" and "never published". */
static volatile unsigned long long g_job_watch_last_job_ms;
void cellSpursDumpNativeJobChains(const char* tag);
static int jobchain_any_running(void);

#ifdef _WIN32
static DWORD WINAPI job_watch_proc(LPVOID opaque)
#else
static void* job_watch_proc(void* opaque)
#endif
{
    (void)opaque;
    static long printed;
    static int stall_episodes, stall_armed = 1;
    for (;;) {
#ifdef _WIN32
        Sleep(2000);
#else
        const struct timespec two_s = {2, 0};
        nanosleep(&two_s, NULL);
#endif
        const unsigned long long now = job_watch_now_ms();
        {
            const unsigned long long last = g_job_watch_last_job_ms;
            if (last && now - last > 6000ull) {
                if (stall_armed && stall_episodes < 3 &&
                    jobchain_any_running()) {
                    ++stall_episodes;
                    stall_armed = 0;
                    fprintf(stderr,
                            "[job-stall] no job entered run_job for %llums "
                            "with a live running chain — dumping chains "
                            "(episode %d)\n",
                            now - last, stall_episodes);
                    cellSpursDumpNativeJobChains("job-stall");
                    yz_spurs_dump_tasksets("job-stall");
                    {
                        extern void yz_spu_dump_all_ctx(const char*);
                        yz_spu_dump_all_ctx("job-stall");
                    }
                    /* Handoff-ordering ring (STATUS 2026-08-06): the stall IS
                     * the trigger of record — dump the pre-stall event tail.
                     * One-shot inside; a later RSX-stall dump becomes a no-op. */
                    yz_frontier_trace_dump(2u);
                    fflush(stderr);
                }
            } else if (last) {
                stall_armed = 1;    /* jobs flowed again: re-arm */
            }
        }
        for (int i = 0; i < JOB_INFLIGHT_MAX; ++i) {
            spu_context* c = (spu_context*)g_job_inflight[i].ctx;
            if (!c) continue;
            const unsigned long long age = now - g_job_inflight[i].start_ms;
            if (age < 4000ull) continue;
            if (printed < 96) {
                ++printed;
                fprintf(stderr,
                        "[jobhang] desc=%08X bin=%08X d10=%08X age=%llums "
                        "pc=0x%05X\n",
                        g_job_inflight[i].desc, g_job_inflight[i].bin,
                        g_job_inflight[i].d10, age, c->pc);
                fflush(stderr);
            }
        }
    }
#ifndef _WIN32
    return NULL;
#else
    return 0;
#endif
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
    else if (!job_descriptor_acquire_published(
                 descriptor_ea, (u16)descriptor_size, descriptor_copy))
        return CELL_SPURS_JOB_ERROR_FAULT;
    const u8* const d = descriptor_copy;
    /* 2026-08-05 (audit H1 / task-ledger #16): jobType at d[0x2C] selects
     * the descriptor format; nonzero = the BINARY2 family, whose
     * binaryInfo[10] layout is documented nowhere. We would MISPARSE it as
     * legacy eaBinary+sizeBinary. One-shot probe dumps the first real
     * BINARY2 descriptor seen so its layout can be recovered from the log;
     * until then, refuse the parse honestly instead of loading garbage. */
    if (descriptor_size > 0x2C && d[0x2C] != 0) {
        static int b2_dumped = 0;
        if (!b2_dumped) {
            b2_dumped = 1;
            fprintf(stderr, "[jc-binary2] jobType=%u descriptor=0x%08X — "
                    "header+binaryInfo dump:\n", d[0x2C], descriptor_ea);
            for (u32 o = 0; o < 0x40 && o < descriptor_size; o += 16)
                fprintf(stderr, "[jc-binary2]   +%02X: %08X %08X %08X %08X\n",
                        o, rd32(d + o), rd32(d + o + 4),
                        rd32(d + o + 8), rd32(d + o + 12));
            fflush(stderr);
        }
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    }
    u32 bin_ea = (u32)(rd64(d + 0x00) & ~1ull);
    u32 bin_size = (u32)rd16(d + 0x08) * 16;
    jobchain_ledger_record(jc, 'J', descriptor_ea, bin_size, bin_ea);
    if (native_trace_enabled())
        fprintf(stderr,
                "[native-spurs-trace] job-start descriptor=0x%08X "
                "bin=0x%08X size=0x%X slot=0x%05X\n",
                descriptor_ea, bin_ea, bin_size, jc->alternate_slot);
    if (!bin_ea || !bin_size) {
        fprintf(stderr,
                "[native-spurs] incomplete job descriptor publication: "
                "descriptor=0x%08X bin=0x%08X blocks=0x%X "
                "kind=0x%08X ready=0x%08X\n",
                descriptor_ea, bin_ea, bin_size / 16u,
                rd32(d + 0x10), rd32(d + 0x14));
        fflush(stderr);
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    }
    if (!job_guest_range_valid(bin_ea, bin_size))
        return CELL_SPURS_JOB_ERROR_FAULT;
    const u32 dma_list_size = rd16(d + 0x0a);
    const u32 cache_list_size = rd32(d + 0x24);
    if ((dma_list_size & 7) || (cache_list_size & 7) ||
        dma_list_size + cache_list_size > descriptor_size - 0x30 ||
        cache_list_size / 8 > 4)
        return CELL_SPURS_JOB_ERROR_DESCRIPTOR;
    spu_workload_image image;
    if (!spu_workload_resolve(vm_base + bin_ea, bin_size, &image)) {
        const u64 fingerprint = spu_workload_fingerprint(
            vm_base + bin_ea, bin_size);
        unknown_job_capture(
            descriptor_ea, descriptor_size, bin_ea, bin_size,
            fingerprint, jc->alternate_slot);
        fprintf(stderr,
                "[native-spurs] unregistered job image: descriptor=0x%08X "
                "bin=0x%08X size=0x%X fingerprint=0x%016llX "
                "next-slot=0x%05X; extract/lift this exact binary and add "
                "both native alternate-slot placements\n",
                descriptor_ea, bin_ea, bin_size,
                (unsigned long long)fingerprint,
                jc->alternate_slot);
        fflush(stderr);
        return CELL_SPURS_JOB_ERROR_INVALID_BIN;
    }
    const u32 size_io = rd32(d + 0x14) & 0x3ffffu;
    const u32 size_out = rd32(d + 0x18) & 0x3ffffu;
    const u32 size_stack = rd16(d + 0x1c) ?
                           (u32)rd16(d + 0x1c) * 16u : 8192u;
    const u32 size_scratch = (u32)rd16(d + 0x1e) * 16u;
    const u32 preferred_slot = jc->alternate_slot;
    const u32 fallback_slot =
        preferred_slot == 0x4c00u ? 0xe400u : 0x4c00u;
    u32 slot = preferred_slot;
    if (!job_layout_fits(
            d, bin_size, dma_list_size, cache_list_size, slot)) {
        if (!job_layout_fits(
                d, bin_size, dma_list_size, cache_list_size,
                fallback_slot)) {
            fprintf(stderr,
                    "[native-spurs] job LS layout does not fit either slot: "
                    "descriptor=0x%08X bin=0x%08X/0x%X io=0x%X "
                    "out=0x%X stack=0x%X scratch=0x%X cache-list=0x%X\n",
                    descriptor_ea, bin_ea, bin_size, size_io, size_out,
                    size_stack, size_scratch, cache_list_size);
            return CELL_SPURS_JOB_ERROR_NOMEM;
        }
        slot = fallback_slot;
        {
            /* A large load batch can contain thousands of descriptors with
             * the same valid fallback.  Logging every one materially starves
             * the renderer during the Frontier transition, so retain an
             * exponentially sampled audit trail instead. */
            static atomic_uint fallback_slot_n;
            const unsigned k = atomic_fetch_add(&fallback_slot_n, 1u);
            if (k < 16u || (k & (k + 1u)) == 0u) {
                fprintf(stderr,
                        "[native-spurs] job 0x%08X does not fit preferred "
                        "LS slot 0x%05X; using 0x%05X (#%u)\n",
                        descriptor_ea, preferred_slot, slot, k + 1u);
            }
        }
    }
    spu_context* ctx = (spu_context*)malloc(sizeof(*ctx));
    if (!ctx) return CELL_SPURS_JOB_ERROR_NOMEM;
    spu_context_init(ctx, 0);
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
    u32 cursor = (slot + bin_size + 1023u) & ~1023u;
    const u32 io_ls = job_alloc(&cursor, descriptor_ls, size_io, 1024);
    const u32 out_ls = job_alloc(&cursor, descriptor_ls, size_out, 1024);
    u32 cache_ls[4] = {0, 0, 0, 0};
    u32 cache_count = cache_list_size / 8;
    for (u32 i = 0; i < cache_count; ++i) {
        u64 item = rd64(d + 0x30 + dma_list_size + i * 8);
        /* 2026-08-05 (audit H6): read-only cache elements carry the FULL
         * high word as their size -- the old MFC-list-style 0x7fff mask
         * silently truncated >=32KB caches. Validate instead: 16-multiple,
         * fits in LS. (The io list below keeps its 0x7fff mask -- those ARE
         * MFC list elements with a <=16KB contract enforced there.) */
        u32 item_size = (u32)(item >> 32);
        if (item_size & 15u) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_INVALID_BIN;
        }
        if (item_size > SPU_LS_SIZE) {
            native_spu_context_free(ctx);
            return CELL_SPURS_JOB_ERROR_INVALID_BIN;
        }
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
        (job_io_trace_enabled() && bin_ea == job_io_trace_binary() &&
         rd32(d + 0x10) >= job_io_trace_min_kind())
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
        for (u32 offset = 0; offset < descriptor_size; offset += 16u) {
            fprintf(stderr,
                    "[native-job-io] #%u desc+%03X %08X %08X %08X %08X\n",
                    job_io_trace_index, offset,
                    rd32(d + offset + 0u), rd32(d + offset + 4u),
                    rd32(d + offset + 8u), rd32(d + offset + 12u));
        }
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
        /* A DMA-list element's top bit is the MFC stall-and-notify flag,
         * not part of its 15-bit transfer size.  The flag is legal on a
         * zero-length final element (acceptance run 9 observed the title's
         * exact 0x8000000000000001 sentinel); synchronous host acquisition
         * has already completed by job entry, and hardware ignores a stall
         * on the final element.  Keep rejecting the genuinely reserved bits
         * between that flag and the transfer-size field. */
        if (item_size > 0x4000u || (item_flags & 0x7fff8000u) ||
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

    ctx->native_job_descriptor_ea = descriptor_ea;
    ctx->native_job_binary_ea = bin_ea;
    ctx->native_job_descriptor_ls = descriptor_ls;
    ctx->native_job_descriptor_size = descriptor_size;
    ctx->native_job_context_ls = context_ls;
    ctx->native_job_io_ls = io_ls;
    ctx->native_job_io_size = size_io;

    const u32 job_io_trace_input_hash = job_io_trace_index < 32u
        ? job_io_trace_hash(ctx->ls + io_ls, size_io) : 0;

    ctx->gpr[0]._u32[0] = 0x0a70;
    {
        /* SPU ABI 2.5.1: entry starts 0x30 below the stack boundary.  The
         * initial frame points to the minimal root frame at end-0x10, whose
         * backchain is NULL; end-0x20 is reserved for the entry LR save.
         * Placing r1 at stack_end made an ordinary stqd r0,0x10(r1)
         * overwrite the following LS allocation (Frontier's descriptor when
         * a job layout ended exactly at 0x3f000). */
        const u32 stack_end =
            scratch_stack_ls + size_scratch + size_stack;
        const u32 initial_sp = stack_end - 0x30u;
        const u32 root_sp = stack_end - 0x10u;
        wr32(ctx->ls + initial_sp, root_sp);
        wr32(ctx->ls + root_sp, 0u);
        ctx->gpr[1]._u32[0] = initial_sp;
        /* ABI word element 1 is Available Stack Space. */
        ctx->gpr[1]._u32[1] = size_stack;
    }
    ctx->gpr[3]._u32[0] = context_ls;
    ctx->gpr[4]._u32[0] = descriptor_ls;
    ctx->native_spurs_syscall = job_return_syscall;
    ctx->pc = slot;
    image.entry_pc = slot;
    const u32 jobdone_d10_snap = rd32(d + 0x10);
    const int parity_motion =
        ((u32)(descriptor_ea - 0x401ACB00u) < 0x80000u) &&
        jobdone_d10_snap == 7u;
    static atomic_uint parity_motion_submit_generation;
    const u32 parity_motion_generation = parity_motion
        ? atomic_fetch_add_explicit(
              &parity_motion_submit_generation, 1u,
              memory_order_relaxed) + 1u
        : 0u;
    if (parity_motion) {
        const u32 ticket = (descriptor_ea - 0x401ACB00u) >> 7;
        const u32 placement =
            ((u32)(image.image_id & 0xff) << 20) | (slot & 0xfffffu);
        ctx->parity_motion_generation = parity_motion_generation;
        yz_frontier_trace_emit(
            YZ_FT_PARITY_MOTION, jc->wid, 0u,
            guest_ea(jc->sync.key), descriptor_ea,
            parity_motion_generation, placement,
            ticket, jobdone_d10_snap);
    }
    yz_frontier_trace_emit(
        YZ_FT_JOB, jc->wid, 0u,
        descriptor_ea, bin_ea, jobdone_d10_snap,
        rd32(vm_base + descriptor_ea + 0x10u), 0u,
        atomic_load_explicit(&jc->ledger_next, memory_order_relaxed));
    /* Register in flight + [jobstart] for the rare submission kinds (>=6:
     * the boots-26/27 stuck kind and the motion job) so "started but hung"
     * is distinguishable from "never consumed". Racy slot claim is fine —
     * diagnostic only, worst case one lost entry. */
    g_job_watch_last_job_ms = job_watch_now_ms();
    int inflight_idx = -1;
    for (int ii = 0; ii < JOB_INFLIGHT_MAX; ++ii) {
        if (!g_job_inflight[ii].ctx) {
            g_job_inflight[ii].desc = descriptor_ea;
            g_job_inflight[ii].bin = bin_ea;
            g_job_inflight[ii].d10 = jobdone_d10_snap;
            g_job_inflight[ii].start_ms = job_watch_now_ms();
            g_job_inflight[ii].ctx = ctx;
            inflight_idx = ii;
            break;
        }
    }
    if (inflight_idx >= 0 && !atomic_exchange(&g_job_watch_started, 1)) {
        nthread watch_thread;
        nthread_create_spu(&watch_thread, job_watch_proc, NULL);
    }
    if (((u32)(descriptor_ea - 0x401ACB00u) < 0x80000u) &&
        jobdone_d10_snap >= 6u) {
        static atomic_uint jobstart_n;
        const unsigned k = atomic_fetch_add(&jobstart_n, 1u);
        if (k < 64u) {
            fprintf(stderr, "[jobstart] desc=%08X bin=%08X d10=%08X\n",
                    descriptor_ea, bin_ea, jobdone_d10_snap);
            fflush(stderr);
        }
    }
    extern volatile unsigned long g_yz_job_dma_put_n;
    extern volatile unsigned long g_yz_job_dma_put_bytes;
    const unsigned long job_put_n0 = g_yz_job_dma_put_n;
    const unsigned long job_put_b0 = g_yz_job_dma_put_bytes;
    yz_frame_dep_spurs_schedule(2u, (u32)image.image_id, jc->wid,
                                descriptor_ea);
    yz_frame_dep_spu_job_start((u32)image.image_id, ctx->spu_id, jc->wid,
                               descriptor_ea);
    int ok = spu_workload_execute(&image, ctx);
    yz_frame_dep_spu_job_complete((u32)image.image_id, ctx->spu_id, jc->wid,
                                  descriptor_ea);
    if (parity_motion) {
        static atomic_uint parity_motion_completion_generation;
        const u32 completion_generation =
            atomic_fetch_add_explicit(
                &parity_motion_completion_generation, 1u,
                memory_order_relaxed) + 1u;
        const u32 placement =
            ((u32)(image.image_id & 0xff) << 20) | (slot & 0xfffffu);
        yz_frontier_trace_emit(
            YZ_FT_PARITY_MOTION, jc->wid, 1u,
            guest_ea(jc->sync.key), descriptor_ea,
            parity_motion_generation, completion_generation,
            rd32(vm_base + descriptor_ea + 0x10u), placement);
    }
    if (inflight_idx >= 0) g_job_inflight[inflight_idx].ctx = NULL;
    yz_frontier_trace_emit(
        YZ_FT_JOB, jc->wid, 1u,
        descriptor_ea, bin_ea, jobdone_d10_snap,
        rd32(vm_base + descriptor_ea + 0x10u), ok ? 0u : 1u,
        atomic_load_explicit(&jc->ledger_next, memory_order_relaxed));
    atomic_fetch_add_explicit(
        &g_frontier_job_completion_serial, 1u, memory_order_relaxed);
    /* DECISIVE (2026-08-05 wedge): did this job publish ANYTHING itself?
     * The SPURS contract says a job's outputs reach main memory only via
     * its own DMA. A completed job with zero store-class DMA has produced
     * nothing the game can observe — which is exactly what the retired
     * write-back used to paper over. Counters are cross-thread-summed, so
     * treat a delta as a lower bound / attributable only on quiet lanes. */
    {
        const unsigned long put_n = g_yz_job_dma_put_n - job_put_n0;
        const unsigned long put_b = g_yz_job_dma_put_bytes - job_put_b0;
        const int ring_load =
            ((u32)(descriptor_ea - 0x401ACB00u) < 0x80000u) &&
            jobdone_d10_snap >= 6u;
        if (ring_load) {
            static atomic_uint jobput_n, jobput_zero_n;
            const unsigned k = atomic_fetch_add(
                put_n ? &jobput_n : &jobput_zero_n, 1u);
            if (k < 48u) {
                fprintf(stderr,
                        "[jobput]%s desc=%08X bin=%08X d10=%X ok=%d "
                        "puts=%lu bytes=%lu\n",
                        put_n ? "" : " ZERO-OUTPUT", descriptor_ea, bin_ea,
                        jobdone_d10_snap, ok, put_n, put_b);
                fflush(stderr);
            }
        }
    }

    /* [jobdone] completion-protocol probe (2026-08-05, boots 25/26 wedge
     * root-cause). The title's PPU polls its ticket-ring slot word
     * (descriptor+0x10) for job completion, and both no-write-back boots
     * froze in that poll (func_00456BC0 spin, ring 0x401ACB00) — a face NO
     * pre-fix wedge boot shows. Measure who delivers the zero: the job's
     * own output DMA (guest flips by job end), a later actor (guest still
     * set at job end while the boot stays healthy), or nobody (the wedge).
     * io0 identifies whether the slot itself is an inout item — the path
     * by which the retired write-back used to publish the LS-side zero. */
#if !defined(YZ_PERF_CLEAN)
    if (g_jobdone_diag_output) {
        FILE* const output = g_jobdone_diag_output;
        static atomic_uint jobdone_any_n, jobdone_ring_n, jobdone_stale_n;
        const u32 d10_now = rd32(vm_base + descriptor_ea + 0x10);
        const u32 d10_ls = rd32(ctx->ls + descriptor_ls + 0x10);
        const u64 io0 = io_count ? rd64(d + 0x30) : 0;
        const u32 io0_ea = (u32)io0;
        const u32 io0_size = (u32)(io0 >> 32) & 0x7fffu;
        const int io0_covers_d10 =
            io0_size >= 0x14 && io0_ea <= descriptor_ea &&
            descriptor_ea + 0x14 <= io0_ea + io0_size;
        const u32 io0_ls10 = io0_covers_d10
            ? rd32(ctx->ls + io_ls + (io0_ea & 15u) +
                   (descriptor_ea - io0_ea) + 0x10)
            : 0xdeaddead;
        /* MEASURED ring (pool @0x015F4410): base 0x401ACB00, cap 0x1000
         * slots * 128B. d+0x10 is useInOutBuffer for every OTHER job, so
         * "still set at exit" is only anomalous for ring descriptors.
         * d10 snap > 1 marks the LOAD-era submissions (measured 6/7 vs the
         * per-frame kind 1) — boots 26/27 wedged on one of those never
         * executing, so they are sampled on their own generous cap. */
        static atomic_uint jobdone_load_n, jobdone_total_n;
        const int in_ring = (u32)(descriptor_ea - 0x401ACB00u) < 0x80000u;
        const int load_kind = in_ring && jobdone_d10_snap >= 6u;
        const int stale = ok && in_ring && jobdone_d10_snap &&
                          d10_now == jobdone_d10_snap;
        const unsigned k = atomic_fetch_add(
            stale ? &jobdone_stale_n :
            load_kind ? &jobdone_load_n :
            in_ring ? &jobdone_ring_n : &jobdone_any_n, 1u);
        if (k < (stale || load_kind ? 64u : 24u)) {
            fprintf(output,
                    "[jobdone]%s desc=%08X bin=%08X ok=%d d10 snap=%08X "
                    "now=%08X lsdesc=%08X io0=%08X/%X io0ls10=%08X\n",
                    stale ? " STALE" : load_kind ? " ringload" :
                    in_ring ? " ring" : "",
                    descriptor_ea, bin_ea, ok,
                    jobdone_d10_snap, d10_now, d10_ls,
                    io0_ea, io0_size, io0_ls10);
        }
        {
            const unsigned t = atomic_fetch_add(&jobdone_total_n, 1u);
            if ((t & 2047u) == 2047u) {
                fprintf(output,
                        "[jobdone] totals jobs=%u ring=%u ringload=%u "
                        "stale=%u\n", t + 1u,
                        atomic_load(&jobdone_ring_n),
                        atomic_load(&jobdone_load_n),
                        atomic_load(&jobdone_stale_n));
            }
        }
    }
#endif

    if (job_io_trace_index < 32u) {
        fprintf(stderr,
                "[native-job-io] #%u result ok=%d pc=%05X tag=%u "
                "input=%08X->%08X output=%08X\n",
                job_io_trace_index, ok, ctx->pc,
                rd32(ctx->ls + context_ls + 0x24),
                job_io_trace_input_hash,
                job_io_trace_hash(ctx->ls + io_ls, size_io),
                job_io_trace_hash(ctx->ls + out_ls, size_out));
        if (!ok) {
            for (u32 reg = 0; reg < 128u; ++reg) {
                fprintf(stderr,
                        "[native-job-io] #%u fail-r%03u="
                        "%08X_%08X_%08X_%08X\n",
                        job_io_trace_index, reg,
                        ctx->gpr[reg]._u32[0], ctx->gpr[reg]._u32[1],
                        ctx->gpr[reg]._u32[2], ctx->gpr[reg]._u32[3]);
            }
        }
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

    /* NO automatic inout write-back — contract-corrected 2026-08-05.
     * The SPURS job module does NOT copy the input-output buffer back to
     * main memory. useInOutBuffer only selects a WRITABLE gather buffer;
     * data output is the JOB's own responsibility over the context dmaTag,
     * and the chain merely fences that tag group. ORACLE(Job Reference:
     * CellSpursJobContext2.dmaTag "DMA tag IDs to be used for data output
     * from a job"; jobchain attr tag1/tag2 "DMA tag IDs to be used for job
     * output DMAs"; SYNC "also waits for DMA output to complete") and
     * ORACLE(SDK JDL codegen jdl/02_basic_buffers/_jobTransformVariable_
     * spurs_job.cpp: the useInOutBuffer=1 job PUTs its own io buffer with
     * the context tag). Our former unconditional write-back republished the
     * job's LS WORKING COPY over guest memory; boot 24 MEASURED it clobbering
     * the motion descriptor's EA pointers with the job's LS-relocated working
     * pointers (the dialogue-scene crash — every pointer field off by exactly
     * item_ea - item_ls, and the PPU builder func_00E88B88 never runs again).
     * Jobs' real output DMAs already execute inside spu_workload_execute and
     * are drained by job_return_syscall before this point, so the removal
     * loses nothing. YZ_JOB_INOUT_WB=1 forces the legacy write-back for A/B. */
    /* [jobio-diff] (2026-08-05 wedge root-cause, boots 25-27): with the
     * fabricated write-back retired, log what each completed inout job left
     * CHANGED in its LS io buffer relative to guest — exactly the bytes the
     * old write-back would have published. A diff is either (a) job-local
     * working state never meant to be published (the boot-24 pointer
     * clobber) or (b) protocol words the game expects a writer to deliver
     * (chain commands / completion words / result pointers) — (b) is the
     * wedge suspect. Diagnostic only; capped; LOAD-era ring jobs always
     * sampled on their own cap. */
    if (ok && rd32(d + 0x10) && !job_inout_wb_forced()) {
        static atomic_uint jobio_diff_n, jobio_diff_load_n;
        /* >= 6: the boot-26/27 stuck kind (6) and the motion job (7); the
         * kind-5 load flood (9k+ in boot 28) must not exhaust the cap. */
        const int diff_load = ((u32)(descriptor_ea - 0x401ACB00u) < 0x80000u)
                              && rd32(d + 0x10) >= 6u;
        io_offset = 0;
        for (u32 i = 0; i < io_count; ++i) {
            const u64 item = rd64(d + 0x30 + i * 8);
            const u32 item_size = (u32)(item >> 32) & 0x7fffu;
            const u32 item_ea = (u32)item;
            const u32 item_ls_offset = io_offset + (item_ea & 15u);
            if (item_size >= 4) {
                u32 diffw = 0, first = 0xffffffffu, last = 0;
                for (u32 o = 0; o + 4 <= item_size; o += 4) {
                    if (rd32(vm_base + item_ea + o) !=
                        rd32(ctx->ls + io_ls + item_ls_offset + o)) {
                        ++diffw;
                        if (first == 0xffffffffu) first = o;
                        last = o;
                    }
                }
                if (diffw) {
                    const unsigned k = atomic_fetch_add(
                        diff_load ? &jobio_diff_load_n : &jobio_diff_n, 1u);
                    if (k < (diff_load ? 64u : 24u)) {
                        fprintf(stderr,
                                "[jobio-diff]%s desc=%08X bin=%08X item=%u "
                                "ea=%08X size=%X diffw=%u first=+%X "
                                "last=+%X g=%08X ls=%08X\n",
                                diff_load ? " LOAD" : "", descriptor_ea,
                                bin_ea, i, item_ea, item_size, diffw,
                                first, last,
                                rd32(vm_base + item_ea + first),
                                rd32(ctx->ls + io_ls + item_ls_offset +
                                     first));
                        fflush(stderr);
                    }
                }
            }
            io_offset += (item_size + 15u) & ~15u;
        }
    }
    if (ok && rd32(d + 0x10) && job_inout_wb_forced()) {
        io_offset = 0;
        for (u32 i = 0; i < io_count; ++i) {
            u64 item = rd64(d + 0x30 + i * 8);
            u32 item_size = (u32)(item >> 32) & 0x7fffu;
            const u32 item_ea = (u32)item;
            const u32 item_ls_offset = io_offset + (item_ea & 15u);
            /* YZ_JOBPUB_WATCH=hexLO[-hexHI] (2026-08-05 frontier): the
             * inout write-back is the MEASURED writer of the fatal motion
             * descriptor -- the page guard caught this copy turning a
             * correctly-rebuilt pointer (0x62B408A0) into an LS-space one
             * (0x0000C520 == io_ls + 0x100 + 0x20). Dump the job identity and
             * the LS-vs-guest bytes so we can tell whether the JOB produced
             * these bytes (our SPU execution left the buffer unconverted) or
             * whether the PUBLISH itself is wrong (stale/over-broad copy).
             * Diagnostic only; no behavior change. */
            if (item_size) {
                static int jpw_lo = -1; static u32 jpw_l = 0, jpw_h = 0;
                if (jpw_lo < 0) {
                    const char* e = getenv("YZ_JOBPUB_WATCH");
                    if (e && *e) {
                        char* end = NULL;
                        jpw_l = (u32)strtoul(e, &end, 16);
                        jpw_h = (end && *end == '-') ? (u32)strtoul(end + 1, NULL, 16)
                                                     : jpw_l + 4u;
                        jpw_lo = 1;
                    } else jpw_lo = 0;
                }
                if (jpw_lo == 1 && item_ea < jpw_h && item_ea + item_size > jpw_l) {
                    const u8* lsp = ctx->ls + io_ls + item_ls_offset;
                    fprintf(stderr,
                            "[jobpub] CLOBBER desc=0x%08X bin=0x%08X item=%u "
                            "ea=0x%08X size=0x%X io_ls=0x%05X off=0x%05X "
                            "useInOut=0x%X ok=%d\n",
                            descriptor_ea, bin_ea, i, item_ea, item_size,
                            io_ls, item_ls_offset, rd32(d + 0x10), ok);
                    for (u32 o = 0; o + 4 <= item_size && o < 0x40; o += 4) {
                        const u32 dst = item_ea + o;
                        if (dst + 4 <= jpw_l || dst >= jpw_h) continue;
                        fprintf(stderr, "[jobpub]   +%02X guest=%08X ls=%08X\n",
                                o, rd32(vm_base + dst), rd32(lsp + o));
                    }
                    fflush(stderr);
                }
                job_publish_copy(item_ea,
                                 ctx->ls + io_ls + item_ls_offset,
                                 item_size);
            }
            io_offset += (item_size + 15u) & ~15u;
        }
    }
    if (job_io_trace_index < 32u) {
        io_offset = 0;
        for (u32 i = 0; i < io_count; ++i) {
            const u64 item = rd64(d + 0x30 + i * 8);
            const u32 item_size = (u32)(item >> 32) & 0x7fffu;
            const u32 item_ea = (u32)item;
            const u32 item_ls_offset = io_offset + (item_ea & 15u);
            if (item_size) {
                const u32 guest_hash =
                    job_io_trace_hash(vm_base + item_ea, item_size);
                const u32 ls_hash = job_io_trace_hash(
                    ctx->ls + io_ls + item_ls_offset, item_size);
                fprintf(stderr,
                        "[native-job-io] #%u final[%u] match=%u "
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
    while (g->count && !atomic_load(&jc->shutdown)) {
        /* S5: re-sync the mirror from guest each wake and wait BOUNDED — a
         * lifted-SPU putllc decrement doesn't signal this condvar, and this
         * was the only untimed guest-dependent wait left (a lost decrement
         * became a permanent jobchain stall instead of 2 ms latency). */
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
    /* NOTE (2026-08-05, boot 32 REFUTATION — do not re-chase): treating
     * 0x0000000800000012 as an executable LWSYNC (its low word does decode
     * as 2|(2<<3)) and advancing after a grace DOES NOT WORK. Boot 32 died
     * at frame 88: during disc-bound phases the producer's marker->command
     * gap is arbitrarily LONG, the scan raced a full lap through marker
     * runs, and the pipeline desynced before the title screen. The marker
     * park below is behaviorally REQUIRED; whatever unsticks the dialogue
     * stall, it is not this. */
    while (jc->waiting_command && rd64(vm_base + pc) == empty_command &&
           !atomic_load(&jc->shutdown)) {
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

/* Effect-pool slot watch (runtime/ppu/ppu_memory.h hook; Akiyama/Hana
 * null-object hunt). Defined here so every runtime-linking binary resolves
 * the symbols; armed by the game runner via YZ_EFFSLOT_WATCH. */
int g_yz_effslot_watch = 0;
void yz_effslot_log(u32 addr, unsigned long long val, void* ra, int width)
{
    static _Atomic long effslot_n = 0;
    long i = atomic_fetch_add(&effslot_n, 1) + 1;
    if (i > 64 && (i & 0x3F)) return;
    fprintf(stderr, "[effslot] #%ld ea=0x%08X w%d val=0x%llX host=%p\n",
            i, addr, width, val, ra);
    fflush(stderr);
}

static int jobchain_any_running(void)
{
    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        JobChainState* jc = &g_jobchains[i];
        if (jc->sync.live && jc->running && !jc->complete &&
            !atomic_load(&jc->shutdown))
            return 1;
    }
    return 0;
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
        /* LIVE guest words (2026-08-05 stall decode): what the game has
         * actually published at the slots we are waiting on, vs the
         * remembered "empty" view we parked against. */
        if (vm_base && jc->current_ea &&
            job_guest_range_valid(jc->current_ea, 8))
            fprintf(stderr,
                    "[native-spurs-ledger]   current guest=%016llX\n",
                    (unsigned long long)rd64(vm_base + jc->current_ea));
        for (u32 p = 0; p < jc->parked_slot_count; ++p) {
            const u32 slot_ea = jc->parked_slot_ea[p];
            if (!vm_base || !job_guest_range_valid(slot_ea, 8)) continue;
            fprintf(stderr,
                    "[native-spurs-ledger]   parked[%u] ea=%08X "
                    "empty=%016llX guest=%016llX\n",
                    p, slot_ea,
                    (unsigned long long)jc->parked_slot_empty[p],
                    (unsigned long long)rd64(vm_base + slot_ea));
        }
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

/* Non-formatting, read-only freeze snapshot.  The side tables are fixed for
 * the process lifetime; racy scalar reads are preferable here to taking a
 * mutex that may itself be part of the wait-for chain. */
void yz_frontier_spurs_snapshot(void)
{
    for (u32 i = 0; i < MAX_SPURS_INSTANCES; ++i) {
        const SpursState* spurs = &g_spurs[i];
        if (!spurs->sync.live) continue;
        yz_frontier_trace_emit(
            YZ_FT_SPURS_WORKLOAD, i, guest_ea(spurs->sync.key),
            spurs->active_wkl_mask, spurs->runnable_wkl_mask,
            spurs->nspus, spurs->shutdown ? 1u : 0u,
            spurs->next_wid, spurs->next_spu_num);
    }

    for (u32 i = 0; i < MAX_TASKSETS; ++i) {
        const TasksetState* ts = &g_tasksets[i];
        if (!ts->sync.live || !ts->sync.key) continue;
        const u8* object = (const u8*)ts->sync.key;
        for (u32 word = 0; word < 4; ++word)
            yz_frontier_trace_emit(
                YZ_FT_SPURS_TASKSET, ts->wid, guest_ea(ts->sync.key),
                word,
                rd32(object + 0x00u + word * 4u),
                rd32(object + 0x10u + word * 4u),
                rd32(object + 0x30u + word * 4u),
                rd32(object + 0x40u + word * 4u),
                rd32(object + 0x50u + word * 4u));
        for (u32 id = 0; id < CELL_SPURS_MAX_TASK; ++id) {
            const TaskState* task = &ts->tasks[id];
            if (!task->thread_valid && !task->complete && !task->waiting &&
                !task->signalled)
                continue;
            const u32 flags =
                (task->thread_valid ? 1u : 0u) |
                (task->complete ? 2u : 0u) |
                (task->joined ? 4u : 0u) |
                (task->waiting ? 8u : 0u) |
                (task->signalled ? 16u : 0u) |
                (task->reaping ? 32u : 0u);
            yz_frontier_trace_emit(
                YZ_FT_SPURS_TASK, ts->wid, id,
                guest_ea(ts->sync.key), flags, task->context_ea,
                (u32)task->image.image_id, (u32)task->exit_code,
                task->idle_poll_count);
        }
    }

    for (u32 i = 0; i < MAX_JOBCHAINS; ++i) {
        const JobChainState* jc = &g_jobchains[i];
        if (!jc->sync.live) continue;
        const u32 flags =
            (jc->running ? 1u : 0u) |
            (jc->complete ? 2u : 0u) |
            (jc->waiting_command ? 4u : 0u) |
            (atomic_load_explicit(&jc->shutdown, memory_order_relaxed) ? 8u : 0u);
        yz_frontier_trace_emit(
            YZ_FT_JOBCHAIN, jc->wid, guest_ea(jc->sync.key),
            'S', jc->current_ea, jc->entry_ea,
            ((jc->command_snapshot_index & 0xffffu) << 16) |
                (jc->command_snapshot_count & 0xffffu),
            (u32)jc->error, flags);

        /* Map every pending completion descriptor back to the persistent JOB
         * command slot responsible for consuming it. */
        for (u32 slot = 0; slot < jc->command_state_count; ++slot) {
            const u32 slot_ea = jc->command_state_ea[slot];
            if (!job_guest_range_valid(slot_ea, 8u)) continue;
            const u64 command = rd64(vm_base + slot_ea);
            if (!command || (command & 7u) != 0u) continue;
            const u32 desc = (u32)command;
            if (desc > 0xffffffebu ||
                !job_guest_range_valid(desc + 0x10u, 8u))
                continue;
            const u32 actual = rd32(vm_base + desc + 0x10u);
            if (!actual) continue;
            yz_frontier_trace_emit(
                YZ_FT_COMPLETION, jc->wid, 4u,
                desc, desc + 0x10u, actual, 0u,
                rd32(vm_base + desc + 0x14u), slot_ea);
        }
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
        int acquired_bridge_publication = 0;
        int acquire_call_tail = 0;
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
        mx_unlock(&jc->sync.mutex);
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
            if (!jobchain_bridge_disabled()) {
                mx_lock(&jc->sync.mutex);
                jobchain_bridge_watch_ea_locked(jc, (u32)cmd + 0x10);
                mx_unlock(&jc->sync.mutex);
            }
            int rc = run_job(jc, (u32)cmd, jc->descriptor_size,
                             acquired_descriptor);
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
                rc = run_job(jc, descriptor_ea,
                             joblist.descriptor_size, NULL);
                if (rc) return rc;
            }
            pc += 8; jobchain_set_current(jc, pc); continue;
        }
        if (op == 7) {
            if (ext == 127) {                                  /* END */
                /* Q4-A (Job-Reference p.55): the command pointer STOPS at the
                 * END word; a later cellSpursRunJobChain resumes scanning
                 * from it, not from jobChainEntry. Rewinding re-executed
                 * stale commands on circular streams the PPU patches and
                 * re-kicks, so the freshly published head was never run. */
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
    jc->pending_snapshot_sequence = 0;
    /* TITLE-OBSERVED RUN CONTRACT (2026-08-09): each Run starts a fresh scan
     * from jobChainEntry.  A direct A/B at the Akiyama/Hana frontier was
     * decisive: preserving the parked END PC left the title on a blank
     * dialogue line through frame 4703, while entry restart crossed the text
     * transition and reached the following animation by frame 4100.  The
     * standalone overwrite-END interpretation was therefore not the title's
     * live lifecycle.  Preserve END-resume only as a diagnostic lever. */
    {
        static int end_resume = -1;
        if (end_resume < 0) {
            const char* e = getenv("YZ_JC_END_RESUME");
            end_resume = (e && *e == '1') ? 1 : 0;
            if (end_resume)
                fprintf(stderr, "[jc-resume] DIAGNOSTIC: re-run preserves "
                        "the parked END pc instead of jobChainEntry\n");
        }
        if (!end_resume || !jc->current_ea)
            jc->current_ea = jc->entry_ea;
    }
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
#if defined(YZ_PERF_PROFILE) && defined(_WIN32) && !defined(YZ_PERF_CLEAN)
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
    /* S5 (firmware oracle cellSpursJobGuardNotify @0x02018104: lwarx/stwcx
     * on guard[0x00]): the decrement must be an atomic on GUEST memory, not
     * a blind write-back of a host mirror — an SPU-side putllc decrement
     * landing between our read and store was silently overwritten and the
     * guard never released. Decrement under the lockline, re-derive the
     * host mirror from the result. */
    /* NOTE: job_publish_u32 takes the (non-reentrant) lockline itself, so
     * the store is done inline within one critical section. */
    {
        extern int spu_coh_is_reserved(u32);
        extern void spu_coh_notify_write(u32);
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
