#include "cellSpurs.h"
#include "cellSync.h"
#include "spu_workload.h"

#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#define MEM_SIZE (2u * 1024u * 1024u)
static _Alignas(128) uint8_t guest[MEM_SIZE];
uint8_t* vm_base = guest;
static volatile int task_phase;
static volatile int task_abi_ok;
static volatile int event_route_phase;
static volatile uint32_t event_route_ea;
static volatile int one_shot_runs;
static volatile int job_runs;
static volatile int job_abi_ok;
static volatile int job_hold;
static volatile int job_entered;
static volatile uint32_t job_dma_tag_seen_mask;
static volatile int sub_qword_job_runs;
static volatile int large_job_runs;
static volatile uint32_t large_job_slot;
static volatile int large_job_layout_ok;
static volatile int joblist_job_runs;
static volatile int ticket_job_runs;
static volatile int ticket_job_hold;
static volatile int ticket_job_entered;
static volatile int sub_qword_layout_ok;
static volatile int poll_hold_entered;
static volatile int poll_hold_release;
static volatile int poll_workload_result;
static volatile int poll_yield_dispatch;
static volatile int poll_yield_result;
static volatile int poll_idle_count;
static volatile int poll_idle_result;
static volatile int poll_dma_wait_count;
static volatile uint32_t poll_dma_wait_mask;
static volatile uint32_t poll_dma_wait_input_mask;
static volatile int poll_context_switch_count;
static atomic_flag test_lockline = ATOMIC_FLAG_INIT;
static _Atomic int test_coh_reserved;
static _Atomic uint32_t test_lockline_acquires;
static _Atomic uint32_t test_coh_notifies;
static _Atomic uint32_t test_coh_last_ea;
static _Atomic int lock_order_hook_armed;
static _Atomic int lock_order_queue_mutex_held;
static _Atomic int lock_order_queue_mutex_release;
static _Atomic int descriptor_snapshot_hook_armed;
static _Atomic int descriptor_snapshot_hook_count;
static uint32_t descriptor_snapshot_hook_ea;
static uint32_t descriptor_snapshot_replacement_ea;
static _Atomic int joblist_publication_hook_armed;
static uint32_t joblist_publication_hook_ea;
static uint32_t joblist_publication_descriptor_ea;
static _Atomic int call_tail_publication_hook_armed;
static uint32_t call_tail_publication_hook_ea;
static _Atomic int pending_ticket_claim_hook_armed;
static uint32_t pending_ticket_claim_hook_slot;
static uint32_t pending_ticket_claim_hook_descriptor;
static volatile int call_tail_job_runs;
extern volatile uint32_t g_native_spurs_ppu_watch_hi;
extern int cellSpursTestProductionMainStorageRange(uint32_t ea, uint64_t size);
extern int cellSpursTestCaptureUnknownJob(
    const char* path, uint32_t descriptor_ea, uint32_t descriptor_size,
    uint32_t binary_ea, uint32_t binary_size, uint64_t fingerprint,
    uint32_t next_slot);
extern int yz_spurs_test_pending_snapshot_count(CellSpursJobChain* object);
extern void cellSpursTestTaskBit(
    void* taskset, uint32_t offset, uint32_t id, int value);

typedef struct EventWaitArgs {
    CellSpursEventFlag* flag;
    uint16_t* bits;
    int32_t rc;
} EventWaitArgs;

#if defined(_WIN32)
static DWORD WINAPI event_wait_thread(LPVOID raw)
#else
static void* event_wait_thread(void* raw)
#endif
{
    EventWaitArgs* args = (EventWaitArgs*)raw;
    args->rc = cellSpursEventFlagWait(
        args->flag, args->bits, CELL_SPURS_EVENT_FLAG_OR);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

uint64_t ppu_timebase_now(void) { return (uint64_t)clock(); }
int spu_coh_is_reserved(uint32_t ea)
{
    (void)ea;
    return atomic_load_explicit(&test_coh_reserved, memory_order_acquire);
}
void spu_coh_notify_write(uint32_t ea)
{
    atomic_store_explicit(&test_coh_last_ea, ea, memory_order_relaxed);
    atomic_fetch_add_explicit(&test_coh_notifies, 1u, memory_order_release);
}
void spu_lockline_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
               &test_lockline, memory_order_acquire)) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    atomic_fetch_add_explicit(
        &test_lockline_acquires, 1u, memory_order_release);
}
void spu_lockline_unlock(void)
{
    atomic_flag_clear_explicit(&test_lockline, memory_order_release);
}
extern void spu_lockline_unlock_then_notify_guest_write(
    int notify, uint32_t ea, uint32_t size);

void yz_spurs_lock_order_test_queue_mutex_held(void)
{
    if (!atomic_load_explicit(&lock_order_hook_armed, memory_order_acquire))
        return;
    atomic_store_explicit(
        &lock_order_queue_mutex_held, 1, memory_order_release);
    while (!atomic_load_explicit(
               &lock_order_queue_mutex_release, memory_order_acquire)) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}
void spu_mfc_unregister(spu_context* ctx) { (void)ctx; }
void spu_mfc_context_switch(spu_context* ctx)
{
    (void)ctx;
    ++poll_context_switch_count;
}
void spu_task_wait_all_dma(spu_context* ctx)
{
    poll_dma_wait_input_mask = ctx->mfc_tag_mask;
    ctx->mfc_tag_mask = UINT32_MAX;
    ctx->mfc_tag_status = UINT32_MAX;
    poll_dma_wait_mask = ctx->mfc_tag_mask;
    ctx->mfc_tag_mask = poll_dma_wait_input_mask;
    ++poll_dma_wait_count;
}
int spu_native_image_executor(spu_context* ctx, int image_id, uint32_t pc)
{
    (void)ctx; (void)image_id; (void)pc;
    return 0;
}

static uint32_t be32(const void* v)
{
    const uint8_t* p = (const uint8_t*)v;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put16(void* v, uint16_t x)
{
    uint8_t* p = (uint8_t*)v; p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)x;
}
static void put32(void* v, uint32_t x)
{
    uint8_t* p = (uint8_t*)v;
    p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x;
}
static void put64(void* v, uint64_t x)
{
    put32(v, (uint32_t)(x >> 32)); put32((uint8_t*)v + 4, (uint32_t)x);
}
void yz_spurs_descriptor_snapshot_test_hook(
    uint32_t ea, uint32_t size, uint32_t attempt)
{
    (void)size;
    if (attempt != 0 || ea != descriptor_snapshot_hook_ea)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &descriptor_snapshot_hook_armed, &expected, 0,
            memory_order_acq_rel, memory_order_acquire))
        return;
    /* Switch the complete DMA element between the first and verification
     * copies.  Stable acquisition must reject that mixed observation and
     * retry until one complete generation is seen. */
    put64(guest + ea + 0x30,
          ((uint64_t)16 << 32) | descriptor_snapshot_replacement_ea);
    atomic_fetch_add_explicit(
        &descriptor_snapshot_hook_count, 1, memory_order_release);
}
void yz_spurs_joblist_publication_test_hook(uint32_t ea, uint32_t attempt)
{
    if (attempt != 0 || ea != joblist_publication_hook_ea)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &joblist_publication_hook_armed, &expected, 0,
            memory_order_acq_rel, memory_order_acquire))
        return;
    /* Reproduce Frontier acceptance run 5: the JOBLIST command is already
     * executable while sizeOfJob/eaJobList still belong to the incomplete
     * generation.  Publish the remaining fields after the first invalid
     * observation; the worker must retry instead of halting the chain. */
    put32(guest + ea + 0x04, 0x40);
    put64(guest + ea + 0x08, joblist_publication_descriptor_ea);
}
void yz_spurs_call_tail_publication_test_hook(uint32_t ea, uint32_t attempt)
{
    if (attempt != 0 || ea != call_tail_publication_hook_ea)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &call_tail_publication_hook_armed, &expected, 0,
            memory_order_acq_rel, memory_order_acquire))
        return;
    /* Reproduce Frontier acceptance run 6: CALL is executable before the
     * dynamically assembled sub-list's final RET has been stored. */
    put64(guest + ea, 0x77ull);
}
void yz_spurs_pending_ticket_claim_test_hook(
    uint32_t slot_ea, uint32_t descriptor_ea)
{
    if (slot_ea != pending_ticket_claim_hook_slot ||
        descriptor_ea != pending_ticket_claim_hook_descriptor)
        return;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &pending_ticket_claim_hook_armed, &expected, 0,
            memory_order_acq_rel, memory_order_acquire))
        return;
    /* Reproduce acceptance run 10's live-command race: once the kernel has
     * selected the persistent command, the producer may immediately recycle
     * its ring slot.  Execution must use the descriptor acquired with that
     * command generation, never an uninitialized/stale fallback buffer. */
    put64(guest + slot_ea, 0x0000000800000012ull);
}
static int fail(const char* what, int line)
{
    fprintf(stderr, "FAIL line %d: %s\n", line, what);
    return 1;
}
#define CHECK(x) do { if (!(x)) return fail(#x, __LINE__); } while (0)

static int test_task_bit_reservation_coherence(void)
{
    uint8_t* taskset = guest + 0x1C000u;
    memset(taskset, 0, 128u);
    atomic_store_explicit(&test_coh_reserved, 0, memory_order_release);
    atomic_store_explicit(&test_lockline_acquires, 0u, memory_order_relaxed);
    atomic_store_explicit(&test_coh_notifies, 0u, memory_order_relaxed);
    atomic_store_explicit(&test_coh_last_ea, 0u, memory_order_relaxed);

    cellSpursTestTaskBit(taskset, 0x40u, 3u, 1);
    CHECK(taskset[0x40] == 0x10u);
    CHECK(atomic_load_explicit(
              &test_lockline_acquires, memory_order_acquire) == 0u);
    CHECK(atomic_load_explicit(
              &test_coh_notifies, memory_order_acquire) == 0u);

    atomic_store_explicit(&test_coh_reserved, 1, memory_order_release);
    cellSpursTestTaskBit(taskset, 0x50u, 4u, 1);
    CHECK(taskset[0x50] == 0x08u);
    CHECK(atomic_load_explicit(
              &test_lockline_acquires, memory_order_acquire) == 1u);
    CHECK(atomic_load_explicit(
              &test_coh_notifies, memory_order_acquire) == 1u);
    CHECK(atomic_load_explicit(
              &test_coh_last_ea, memory_order_acquire) == 0x1C050u);

    cellSpursTestTaskBit(taskset, 0x40u, 3u, 0);
    CHECK(taskset[0x40] == 0u);
    CHECK(atomic_load_explicit(
              &test_lockline_acquires, memory_order_acquire) == 2u);
    CHECK(atomic_load_explicit(
              &test_coh_notifies, memory_order_acquire) == 2u);
    atomic_store_explicit(&test_coh_reserved, 0, memory_order_release);
    return 0;
}

static void make_elf(uint8_t* e)
{
    memset(e, 0, 0x104);
    e[0]=0x7f; e[1]='E'; e[2]='L'; e[3]='F'; e[4]=1; e[5]=2;
    put32(e + 0x18, 0x3000);
    put32(e + 0x1c, 0x34);
    put16(e + 0x2a, 0x20);
    put16(e + 0x2c, 1);
    put32(e + 0x34, 1);
    put32(e + 0x38, 0x100);
    put32(e + 0x3c, 0x3000);
    put32(e + 0x44, 4);
    put32(e + 0x48, 4);
    e[0x100]=0x12; e[0x101]=0x34; e[0x102]=0x56; e[0x103]=0x78;
}

static void synthetic_task(spu_context* ctx)
{
    task_abi_ok =
        ctx->gpr[3]._u32[0] == 0x01020304 &&
        ctx->gpr[3]._u32[1] == 0x11121314 &&
        ctx->gpr[3]._u32[2] == 0x21222324 &&
        ctx->gpr[3]._u32[3] == 0x31323334 &&
        ctx->gpr[4]._u32[0] == 0x11223344 &&
        ctx->gpr[4]._u32[1] == 0x55667788 &&
        ctx->gpr[4]._u32[2] == 0 &&
        ctx->gpr[4]._u32[3] == 0x1000 &&
        be32(ctx->ls + 0x1c8) == ctx->spu_id &&
        be32(ctx->ls + 0x27cc) == ctx->spu_id &&
        be32(ctx->ls + 0x2794) == 0x8000 &&
        be32(ctx->ls + 0x2fb8) == 0x2700;
    ctx->gpr[80] = spu_make_preferred_u32(0xdeadbeef);
    task_phase = 1;
    ctx->gpr[3]._u32[0] = 2; /* WAIT_SIGNAL */
    if (ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque) <= 0) return;
    task_abi_ok = task_abi_ok && ctx->gpr[80]._u32[0] == 0xdeadbeef;
    task_phase = 2;
    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    /* The SDK task exit wrapper saves the public exit code here before it
     * repurposes r4 for the native syscall entry. */
    put32(ctx->ls + 0x2fd0, 0x2468);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static void synthetic_event_route_task(spu_context* ctx)
{
    /* A lifted SDK EventFlagWait preserves only its MFC EAH/EAL address pair
     * here.  Extra buffer/geometry lanes identify the title's unrelated
     * CellSync queue descriptor and must not trigger EventFlag discovery. */
    memset(&ctx->gpr[81], 0, sizeof(ctx->gpr[81]));
    ctx->gpr[81]._u32[0] = 0;
    ctx->gpr[81]._u32[1] = event_route_ea;
    event_route_phase = 1;
    ctx->gpr[3]._u32[0] = 2; /* WAIT_SIGNAL */
    if (ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque) <= 0) return;
    event_route_phase = 2;
    ctx->gpr[3]._u32[0] = 2; /* WAIT_SIGNAL again: duplicate edge guard */
    if (ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque) <= 0) return;
    event_route_phase = 3;
    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    put32(ctx->ls + 0x2fd0, 0x4e46);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static void synthetic_exit_task(spu_context* ctx)
{
    ++one_shot_runs;
    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    put32(ctx->ls + 0x2fd0, (uint32_t)one_shot_runs);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static void synthetic_poll_hold_task(spu_context* ctx)
{
    poll_hold_entered = 1;
    while (!poll_hold_release) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    put32(ctx->ls + 0x2fd0, 0);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static void synthetic_poll_task(spu_context* ctx)
{
    ctx->gpr[3]._u32[0] = 3; /* POLL */
    if (ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque) > 0)
        poll_workload_result = (int)ctx->gpr[3]._u32[0];

    ctx->gpr[3]._u32[0] = 1; /* YIELD */
    poll_yield_dispatch =
        ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
    poll_yield_result = (int)ctx->gpr[3]._u32[0];

    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    put32(ctx->ls + 0x2fd0, 0);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static void synthetic_poll_idle_task(spu_context* ctx)
{
    poll_idle_count = 0;
    poll_idle_result = 0;
    for (int i = 1; i <= 4096; ++i) {
        ctx->gpr[3]._u32[0] = 3; /* POLL */
        if (ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque) <= 0)
            break;
        poll_idle_count = i;
        poll_idle_result = (int)ctx->gpr[3]._u32[0];
        if (poll_idle_result != 0)
            break;
    }
    ctx->gpr[3]._u32[0] = 0; /* EXIT */
    put32(ctx->ls + 0x2fd0, 0);
    (void)ctx->native_spurs_syscall(ctx, ctx->native_spurs_opaque);
}
static uint16_t be16(const void* v)
{
    const uint8_t* p = (const uint8_t*)v;
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* The SPURS job module never writes the io buffer back to main memory: a job
 * publishes its own outputs over the context dmaTag (Job Reference dmaTag
 * "for data output from a job"; the SDK JDL codegen PUTs its inout buffer at
 * job end). These synthetic jobs follow the same contract, targeting the EAs
 * in their LS descriptor SNAPSHOT — which is what keeps the descriptor-reuse
 * tests honest. Jobs run as host functions in this harness, so the PUT is a
 * host store + publication notify; the real MFC transport is exercised by
 * tests/spu_atomics_stress and the title. */
extern void cellSpursNotifyGuestWrite(uint32_t ea, uint32_t size);
static void job_put_io_items(spu_context* ctx)
{
    const uint32_t context_ls = ctx->gpr[3]._u32[0];
    const uint32_t descriptor_ls = ctx->gpr[4]._u32[0];
    if (!be32(ctx->ls + descriptor_ls + 0x10)) return;   /* input-only job */
    const uint32_t io_ls = be32(ctx->ls + context_ls);
    const uint32_t count = be16(ctx->ls + context_ls + 0x18);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t size =
            be32(ctx->ls + descriptor_ls + 0x30 + i * 8) & 0x7fffu;
        const uint32_t ea = be32(ctx->ls + descriptor_ls + 0x30 + i * 8 + 4);
        if (size) {
            memcpy(vm_base + ea, ctx->ls + io_ls + offset + (ea & 15u), size);
            cellSpursNotifyGuestWrite(ea, size);
        }
        offset += (size + 15u) & ~15u;
    }
}

/* Link stub: cellSync.c's frozen-ticket forensics dumps every runner thread
 * (yakuza/main.cpp). This harness never freezes a ticket long enough. */
void yz_dump_all_threads_c(const char* tag) { (void)tag; }

/* Link stub: the job-stall watchdog's all-SPU-ctx pc dump (boot 44 face-B
 * forensics; real one in runtime/spu/spu_channels.c, not linked here). */
void yz_spu_dump_all_ctx(const char* tag) { (void)tag; }

/* Link stubs: run_job's job output-DMA census (counters live in
 * spu_channels.c in the full runtime; this harness's jobs are host fns). */
volatile unsigned long g_yz_job_dma_put_n = 0;
volatile unsigned long g_yz_job_dma_put_bytes = 0;
static void synthetic_job(spu_context* ctx)
{
    const uint32_t context_ls = ctx->gpr[3]._u32[0];
    const uint32_t descriptor_ls = ctx->gpr[4]._u32[0];
    const uint32_t io_ls = be32(ctx->ls + context_ls);
    const int slot_abi_ok =
        (ctx->gpr[1]._u32[0] == 0x73d0 && io_ls == 0x5000) ||
        (ctx->gpr[1]._u32[0] == 0x10bd0 && io_ls == 0xe800);
    job_abi_ok =
        ctx->gpr[0]._u32[0] == 0x0a70 &&
        slot_abi_ok &&
        ctx->gpr[1]._u32[1] == 0x2000 &&
        be32(ctx->ls + ctx->gpr[1]._u32[0]) ==
            ctx->gpr[1]._u32[0] + 0x20 &&
        be32(ctx->ls + ctx->gpr[1]._u32[0] + 0x20) == 0 &&
        context_ls == 0x4940 &&
        descriptor_ls == 0x3f000 &&
        ctx->ls[context_ls + 0x18] == 0 &&
        ctx->ls[context_ls + 0x19] == 1 &&
        be32(ctx->ls + context_ls + 0x2c) == 0xe000 &&
        be32(ctx->ls + descriptor_ls + 4) == 0xf000 &&
        be32(ctx->ls + context_ls + 0x24) <= 30;
    if (be32(ctx->ls + context_ls + 0x24) <= 1)
        job_dma_tag_seen_mask |=
            1u << be32(ctx->ls + context_ls + 0x24);
    for (uint32_t i = 0; i < 16; ++i) ctx->ls[io_ls + i] ^= 0xff;
    ++job_runs;
    if (job_hold) {
        job_entered = 1;
        while (job_hold) {
#if defined(_WIN32)
            SwitchToThread();
#else
            sched_yield();
#endif
        }
    }
    job_put_io_items(ctx);
}

static void synthetic_ticket_job(spu_context* ctx)
{
    const uint32_t context_ls = ctx->gpr[3]._u32[0];
    const uint32_t descriptor_ea = be32(ctx->ls + context_ls + 0x2c);
    ++ticket_job_runs;
    /* Model the title's persistent dispatcher consuming pending kind 6. */
    if (be32(vm_base + descriptor_ea + 0x10) == 6)
        put32(vm_base + descriptor_ea + 0x10, 0);
    if (ticket_job_hold) {
        ticket_job_entered = 1;
        while (ticket_job_hold) {
#if defined(_WIN32)
            SwitchToThread();
#else
            sched_yield();
#endif
        }
    }
}

static void synthetic_sub_qword_job(spu_context* ctx)
{
    static const uint32_t ls_offset[4] = {4u, 24u, 44u, 48u};
    static const uint32_t input[4] = {
        0x10203040u, 0x50607080u, 0x90a0b0c0u, 0xd0e0f000u
    };
    const uint32_t context_ls = ctx->gpr[3]._u32[0];
    const uint32_t io_ls = be32(ctx->ls + context_ls);

    sub_qword_layout_ok =
        ctx->gpr[0]._u32[0] == 0x0a70 &&
        context_ls == 0x4940 &&
        ctx->ls[context_ls + 0x18] == 0 &&
        ctx->ls[context_ls + 0x19] == 5;
    for (uint32_t i = 0; i < 4; ++i) {
        if (be32(ctx->ls + io_ls + ls_offset[i]) != input[i])
            sub_qword_layout_ok = 0;
        put32(ctx->ls + io_ls + ls_offset[i], input[i] ^ 0xffffffffu);
    }
    ++sub_qword_job_runs;
    job_put_io_items(ctx);
}

static void synthetic_layout_warmup_job(spu_context* ctx)
{
    (void)ctx;
}

static void synthetic_joblist_job(spu_context* ctx)
{
    (void)ctx;
    ++joblist_job_runs;
}

static void synthetic_call_tail_job(spu_context* ctx)
{
    (void)ctx;
    ++call_tail_job_runs;
}

static void synthetic_large_layout_job(spu_context* ctx)
{
    const uint32_t context_ls = ctx->gpr[3]._u32[0];
    const uint32_t descriptor_ls = ctx->gpr[4]._u32[0];
    const uint32_t initial_sp = ctx->gpr[1]._u32[0];
    large_job_slot = ctx->pc;
    large_job_layout_ok =
        ctx->pc == 0x04c00u &&
        be32(ctx->ls + context_ls + 0x00) == 0x16000u &&
        be32(ctx->ls + context_ls + 0x04) == 0x1e000u &&
        be32(ctx->ls + context_ls + 0x20) == 0x1f000u &&
        descriptor_ls == 0x3f000u &&
        initial_sp == 0x3efd0u &&
        ctx->gpr[1]._u32[1] == 0x8000u &&
        be32(ctx->ls + initial_sp) == 0x3eff0u &&
        be32(ctx->ls + 0x3eff0u) == 0 &&
        be32(ctx->ls + descriptor_ls + 0x10) == 0x13579bdfu;
    /* Emulate the entry prologue that exposed the bug.  Its LR save must be
     * inside the stack allocation, not at descriptor+0x10. */
    put32(ctx->ls + initial_sp + 0x10, ctx->gpr[0]._u32[0]);
    large_job_layout_ok = large_job_layout_ok &&
        be32(ctx->ls + initial_sp + 0x10) == 0x0a70u &&
        be32(ctx->ls + descriptor_ls + 0x10) == 0x13579bdfu;
    ++large_job_runs;
}

static int wait_value(volatile int* value, int expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if (*value == expected) return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}
static int wait_phase(int phase) { return wait_value(&task_phase, phase); }
static void brief_sleep(void)
{
#if defined(_WIN32)
    Sleep(10);
#else
    const struct timespec delay = {0, 10 * 1000 * 1000};
    nanosleep(&delay, NULL);
#endif
}

static int wait_atomic_value(_Atomic int* value, int expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected)
            return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}
static int wait_byte_mask(const volatile uint8_t* value, uint8_t mask,
                          uint8_t expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if ((*value & mask) == expected) return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}
static int wait_be32_value(const void* value, uint32_t expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if (be32(value) == expected) return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}

static int wait_u32_at_least(const volatile uint32_t* value, uint32_t expected)
{
    for (unsigned i = 0; i < 1000000; ++i) {
        if (*value >= expected) return 1;
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    return 0;
}

static int test_layouts(void)
{
    CHECK(sizeof(CellSpurs) == 4096);
    CHECK(sizeof(CellSpurs2) == 8192);
    CHECK(sizeof(CellSpursTaskset) == 0x1900);
    CHECK(sizeof(CellSpursTaskset2) == 0x2900);
    CHECK(sizeof(CellSpursEventFlag) == 128);
    CHECK(sizeof(CellSpursJobChain) == 272);
    return 0;
}

static int test_native_syscall_return_depth(void)
{
    static spu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.host_depth = 1;
    CHECK(!spu_return_needs_dispatch(&ctx));
    ctx.host_depth = 0;
    CHECK(spu_return_needs_dispatch(&ctx));
    return 0;
}

static int test_multiple_instances(void)
{
    CellSpurs* first = (CellSpurs*)(guest + 0x12000);
    CellSpurs* second = (CellSpurs*)(guest + 0x14000);
    CellSpursAttribute* first_attr =
        (CellSpursAttribute*)(guest + 0x16000);
    CellSpursAttribute* second_attr =
        (CellSpursAttribute*)(guest + 0x16800);
    CHECK(cellSpursAttributeInitialize(first_attr, 1, 100, 1000, 0) == 0);
    CHECK(cellSpursAttributeInitialize(second_attr, 3, 200, 2000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(first, first_attr) == 0);
    CHECK(cellSpursInitializeWithAttribute(second, second_attr) == 0);
    CHECK(first->bytes[0x76] == 1);
    CHECK(second->bytes[0x76] == 3);
    CHECK(cellSpursFinalize(first) == 0);
    CHECK(cellSpursFinalize(second) == 0);
    return 0;
}

static int test_task_poll_workload_yield(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x1000);
    CellSpursTaskset2* poll_ts = (CellSpursTaskset2*)(guest + 0x3000);
    CellSpursTaskset2* hold_ts = (CellSpursTaskset2*)(guest + 0x5000);
    CellSpursTaskset2* idle_ts = (CellSpursTaskset2*)(guest + 0x9000);
    CellSpursAttribute* attr = (CellSpursAttribute*)(guest + 0x7000);
    CellSpursTasksetAttribute2* taskset_attr =
        (CellSpursTasksetAttribute2*)(guest + 0x7400);
    uint8_t* hold_elf = guest + 0x8000;
    uint8_t* poll_elf = guest + 0x8200;
    uint8_t* idle_elf = guest + 0x8600;
    CellSpursTaskId* hold_id = (CellSpursTaskId*)(guest + 0x8400);
    CellSpursTaskId* poll_id = (CellSpursTaskId*)(guest + 0x8404);
    int32_t* exit_code = (int32_t*)(guest + 0x8408);
    CellSpursTaskId* idle_id = (CellSpursTaskId*)(guest + 0x840c);
    CellSpursTaskBinInfo bin;
    CellSpursTaskArgument arg;

    make_elf(hold_elf);
    make_elf(poll_elf);
    make_elf(idle_elf);
    poll_elf[0x103] ^= 0x5a;
    idle_elf[0x102] ^= 0xa5;
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(hold_elf, 0x104), 0x104,
              synthetic_poll_hold_task, "synthetic-poll-hold-task"));
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(poll_elf, 0x104), 0x104,
              synthetic_poll_task, "synthetic-poll-task"));
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(idle_elf, 0x104), 0x104,
              synthetic_poll_idle_task, "synthetic-poll-idle-task"));

    CHECK(cellSpursAttributeInitialize(attr, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, attr) == 0);
    _cellSpursTasksetAttribute2Initialize(taskset_attr, 1);
    CHECK(cellSpursCreateTaskset2(spurs, hold_ts, taskset_attr) == 0);
    CHECK(cellSpursCreateTaskset2(spurs, poll_ts, taskset_attr) == 0);
    CHECK(be32(hold_ts->bytes + 0x74) == 0);
    CHECK(be32(poll_ts->bytes + 0x74) == 1);

    memset(&bin, 0, sizeof(bin));
    memset(&arg, 0, sizeof(arg));
    put64((uint8_t*)&bin + 0x00, 0x8000);
    poll_hold_entered = 0;
    poll_hold_release = 0;
    poll_workload_result = -1;
    poll_yield_dispatch = 0;
    poll_yield_result = -1;
    poll_dma_wait_count = 0;
    poll_dma_wait_mask = 0;
    poll_dma_wait_input_mask = 0;
    poll_context_switch_count = 0;
    CHECK(cellSpursCreateTask2WithBinInfo(
              hold_ts, hold_id, &bin, &arg, NULL, "poll-hold", NULL) == 0);
    CHECK(wait_value(&poll_hold_entered, 1));

    put64((uint8_t*)&bin + 0x00, 0x8200);
    CHECK(cellSpursCreateTask2WithBinInfo(
              poll_ts, poll_id, &bin, &arg, NULL, "poll", NULL) == 0);
    /* A peer task which is already RUNNING is not selectable work and must
     * not make this task's first poll report a workload yield. */
    CHECK(wait_value(&poll_workload_result, 0));
    poll_hold_release = 1;

    CHECK(cellSpursJoinTask2(poll_ts, be32(poll_id), exit_code) == 0);
    CHECK(cellSpursJoinTask2(hold_ts, be32(hold_id), exit_code) == 0);
    CHECK(poll_workload_result == 0);
    CHECK(poll_yield_dispatch == 1);
    CHECK(poll_yield_result == 0);
    CHECK(poll_dma_wait_count == 2);
    CHECK(poll_dma_wait_mask == UINT32_MAX);
    CHECK(poll_dma_wait_input_mask == 0);
    CHECK(poll_context_switch_count == 1);
    CHECK(cellSpursDestroyTaskset2(poll_ts) == 0);
    CHECK(cellSpursDestroyTaskset2(hold_ts) == 0);

    CHECK(cellSpursCreateTaskset2(spurs, idle_ts, taskset_attr) == 0);
    CHECK(be32(idle_ts->bytes + 0x74) == 2);
    put64((uint8_t*)&bin + 0x00, 0x8600);
    poll_idle_count = -1;
    poll_idle_result = -1;
    CHECK(cellSpursCreateTask2WithBinInfo(
              idle_ts, idle_id, &bin, &arg, NULL,
              "poll-idle", NULL) == 0);
    CHECK(cellSpursJoinTask2(idle_ts, be32(idle_id), exit_code) == 0);
    CHECK(poll_idle_result == 0);
    CHECK(poll_idle_count == 4096);
    CHECK(poll_dma_wait_count == 2 + poll_idle_count);
    CHECK(poll_context_switch_count == 1);
    CHECK(cellSpursDestroyTaskset2(idle_ts) == 0);

    /* Workload IDs are a 32-entry live set, not a lifetime counter.  Reusing
     * one destroyed taskset must remain possible past a full ID rotation. */
    for (uint32_t iteration = 0; iteration < 40; ++iteration) {
        CHECK(cellSpursCreateTaskset2(spurs, idle_ts, taskset_attr) == 0);
        CHECK(be32(idle_ts->bytes + 0x74) < 32);
        CHECK(cellSpursDestroyTaskset2(idle_ts) == 0);
    }
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_task_event_queue(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x1000);
    CellSpursTaskset2* ts = (CellSpursTaskset2*)(guest + 0x3000);
    CellSpursAttribute* attr = (CellSpursAttribute*)(guest + 0x6000);
    CellSpursTasksetAttribute2* taskset_attr =
        (CellSpursTasksetAttribute2*)(guest + 0x6400);
    CellSpursEventFlag* ef = (CellSpursEventFlag*)(guest + 0x6800);
    CellSpursEventFlag* iwl_ef = (CellSpursEventFlag*)(guest + 0x6880);
    CellSpursEventFlag* raw_ef = (CellSpursEventFlag*)(guest + 0x6c00);
    CellSpursQueue* push_queue = (CellSpursQueue*)(guest + 0x6900);
    CellSpursQueue* pop_queue = (CellSpursQueue*)(guest + 0x6a00);
    CellSpursLFQueue* lf_queue = (CellSpursLFQueue*)(guest + 0x6b00);
    CellSpursLFQueue* lf_wait_queue = (CellSpursLFQueue*)(guest + 0x7300);
    uint8_t* push_data = guest + 0x7000;
    uint8_t* pop_data = guest + 0x7100;
    uint8_t* lf_data = guest + 0x7200;
    uint8_t* lf_wait_data = guest + 0x7400;
    uint8_t* elf = guest + 0x8000;
    uint8_t* task_context = guest + 0x18000;
    CellSpursTaskId* id = (CellSpursTaskId*)(guest + 0x9000);
    int32_t* exit_code = (int32_t*)(guest + 0x9004);
    uint16_t* bits = (uint16_t*)(guest + 0x9010);
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    make_elf(elf);
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(elf, 0x104), 0x104,
              synthetic_task, "synthetic-task"));
    CHECK(cellSpursAttributeInitialize(attr, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, attr) == 0);
    _cellSpursTasksetAttribute2Initialize(taskset_attr, 1);
    put64(taskset_attr->bytes + 0x08, 0x1122334455667788ull);
    CHECK(cellSpursCreateTaskset2(spurs, ts, taskset_attr) == 0);
    task_phase = 0;
    task_abi_ok = 0;
    {
        CellSpursTaskBinInfo bin;
        CellSpursTaskArgument arg;
        memset(&bin, 0, sizeof(bin)); memset(&arg, 0, sizeof(arg));
        put64((uint8_t*)&bin + 0x00, 0x8000);
        put32((uint8_t*)&bin + 0x08, 1024);
        put32(arg.bytes + 0x00, 0x01020304);
        put32(arg.bytes + 0x04, 0x11121314);
        put32(arg.bytes + 0x08, 0x21222324);
        put32(arg.bytes + 0x0c, 0x31323334);
        CHECK(cellSpursCreateTask2WithBinInfo(ts, id, &bin, &arg,
                                             task_context, "task", NULL) == 0);
    }
    CHECK(wait_phase(1));
    CHECK(task_abi_ok);
    CHECK(wait_byte_mask(ts->bytes + 0x50, 0x80, 0x80));
    CHECK((ts->bytes[0x00] & 0x80) == 0);
    CHECK((ts->bytes[0x10] & 0x80) == 0);
    /* Task-Reference p.38: a still-running task answers AGAIN (BUSY is
     * reserved for "another thread is already waiting"); the old BUSY
     * expectation pinned our pre-2026-08-04 out-of-contract return. */
    CHECK(cellSpursTryJoinTask2(ts, be32(id), exit_code) == CELL_SPURS_TASK_ERROR_AGAIN);
    CHECK(_cellSpursSendSignal((CellSpursTaskset*)ts, be32(id)) == 0);
    CHECK(cellSpursJoinTask2(ts, be32(id), exit_code) == 0);
    CHECK(task_phase == 2);
    CHECK(be32(exit_code) == 0x2468);
    CHECK(be32(task_context + 0x20) == 0xdeadbeef);

    /* An IWL event flag stores a SPURS owner plus per-slot workload/task
     * identity.  PPU set must resolve that pair back to the actual taskset;
     * treating the SPURS object at 0x70 as a taskset loses the wakeup. */
    task_phase = 0;
    task_abi_ok = 0;
    poll_dma_wait_count = 0;
    {
        CellSpursTaskBinInfo bin;
        CellSpursTaskArgument arg;
        memset(&bin, 0, sizeof(bin)); memset(&arg, 0, sizeof(arg));
        put64((uint8_t*)&bin + 0x00, 0x8000);
        put32((uint8_t*)&bin + 0x08, 1024);
        put32(arg.bytes + 0x00, 0x01020304);
        put32(arg.bytes + 0x04, 0x11121314);
        put32(arg.bytes + 0x08, 0x21222324);
        put32(arg.bytes + 0x0c, 0x31323334);
        CHECK(cellSpursCreateTask2WithBinInfo(ts, id, &bin, &arg,
                                             task_context + 0x400,
                                             "guest-signal-task", NULL) == 0);
    }
    CHECK(wait_phase(1));
    CHECK(task_abi_ok);
    CHECK(wait_value(&poll_dma_wait_count, 1)); /* WAIT_SIGNAL drains before save */
    {
        const uint32_t task_id = be32(id);
        const uint8_t mask = (uint8_t)(0x80u >> (task_id & 7));
        CHECK(wait_byte_mask(ts->bytes + 0x50 + task_id / 8, mask, mask));
        CHECK((ts->bytes[0x00 + task_id / 8] & mask) == 0);
        CHECK((ts->bytes[0x10 + task_id / 8] & mask) == 0);
        CHECK(_cellSpursEventFlagInitialize(
                  spurs, NULL, iwl_ef,
                  CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL,
                  CELL_SPURS_EVENT_FLAG_PPU2SPU) == 0);
        put16(iwl_ef->bytes + 0x08, 0x8000);
        put16(iwl_ef->bytes + 0x10, 0x0001);
        iwl_ef->bytes[0x50] = (uint8_t)task_id;
        iwl_ef->bytes[0x60] = (uint8_t)be32(ts->bytes + 0x74);
        CHECK(cellSpursEventFlagSet(iwl_ef, 0x0001) == 0);
        CHECK(cellSpursJoinTask2(ts, task_id, exit_code) == 0);
        CHECK((ts->bytes[0x40 + task_id / 8] & mask) == 0);
    }
    CHECK(task_phase == 2);
    CHECK(be32(exit_code) == 0x2468);

    /* Lifted SPU EventFlagSet does not call the HLE Set entry point.  It
     * publishes the event line through MFC PUTLLC, so the exact guest-write
     * router must turn a newly-pending task slot into the SDK depth-one task
     * signal.  CLEAR_AUTO can leave event bits at zero, making +0x02 the
     * only observable state edge.  An unchanged pending bit must not signal
     * the task a second time; clearing and re-arming it creates a new edge. */
    {
        uint8_t* route_elf = guest + 0x1c000;
        CellSpursTaskBinInfo bin;
        CellSpursTaskArgument arg;
        uint32_t task_id;
        uint8_t task_mask;
        make_elf(route_elf);
        route_elf[0x102] ^= 0x40;
        spu_workload_reset();
        CHECK(spu_workload_register_direct(
                  spu_workload_fingerprint(route_elf, 0x104), 0x104,
                  synthetic_event_route_task, "synthetic-event-route-task"));
        memset(&bin, 0, sizeof(bin));
        memset(&arg, 0, sizeof(arg));
        put64((uint8_t*)&bin + 0x00, 0x1c000);
        put32((uint8_t*)&bin + 0x08, 1024);
        /* Reproduce a flag initialized entirely by lifted guest code.  It is
         * deliberately absent from the HLE event registry before the task's
         * semantic EventFlagWait handoff discovers it. */
        memset(raw_ef, 0, sizeof(*raw_ef));
        raw_ef->bytes[0x0c] = 0xff;
        raw_ef->bytes[0x0d] = 1; /* IWL: owner at +0x70 is SPURS */
        raw_ef->bytes[0x0e] = CELL_SPURS_EVENT_FLAG_ANY2ANY;
        raw_ef->bytes[0x0f] = CELL_SPURS_EVENT_FLAG_CLEAR_AUTO;
        put64(raw_ef->bytes + 0x70, 0x1000);
        put16(raw_ef->bytes + 0x08, 0x8000); /* used slot 0 */
        put16(raw_ef->bytes + 0x10, 0x0001); /* wait mask */
        raw_ef->bytes[0x50] = 0;             /* next reclaimed task id */
        raw_ef->bytes[0x60] = (uint8_t)be32(ts->bytes + 0x74);
        event_route_ea = 0x6c00;
        event_route_phase = 0;
        CHECK(cellSpursCreateTask2WithBinInfo(
                  ts, id, &bin, &arg, task_context + 0x800,
                  "event-route-task", NULL) == 0);
        task_id = be32(id);
        CHECK(task_id == 0);
        task_mask = (uint8_t)(0x80u >> (task_id & 7));
        CHECK(wait_value(&event_route_phase, 1));
        CHECK(wait_byte_mask(ts->bytes + 0x50 + task_id / 8,
                             task_mask, task_mask));

        put16(raw_ef->bytes + 0x30, 0x0001); /* received bits */
        put16(raw_ef->bytes + 0x02, 0x8000); /* lifted Set publish */
        atomic_thread_fence(memory_order_release);
        cellSpursNotifyGuestWrite(0x6c00, 128);
        CHECK(wait_value(&event_route_phase, 2));
        CHECK(wait_byte_mask(ts->bytes + 0x50 + task_id / 8,
                             task_mask, task_mask));

        /* The same publication is not another event. */
        cellSpursNotifyGuestWrite(0x6c00, 128);
        brief_sleep();
        CHECK(event_route_phase == 2);
        CHECK((ts->bytes[0x50 + task_id / 8] & task_mask) == task_mask);

        /* The SPU waiter consumes pending before it can be re-armed. */
        put16(raw_ef->bytes + 0x02, 0);
        atomic_thread_fence(memory_order_release);
        cellSpursNotifyGuestWrite(0x6c00, 128);
        brief_sleep();
        CHECK(event_route_phase == 2);
        put16(raw_ef->bytes + 0x02, 0x8000);
        atomic_thread_fence(memory_order_release);
        cellSpursNotifyGuestWrite(0x6c00, 128);
        CHECK(cellSpursJoinTask2(ts, task_id, exit_code) == 0);
        CHECK(event_route_phase == 3);
        CHECK(be32(exit_code) == 0x4e46);
    }

    /* A taskset has 128 concurrent IDs.  Joined one-shot tasks must release
     * their ID so a long-lived taskset can create more than 128 tasks over
     * its lifetime (the SDK vision samples use this create/join pattern per
     * frame). */
    {
        uint8_t* one_shot_elf = guest + 0x1c000;
        CellSpursTaskBinInfo bin;
        CellSpursTaskArgument arg;
        make_elf(one_shot_elf);
        one_shot_elf[0x103] ^= 0x01;
        memset(&bin, 0, sizeof(bin));
        memset(&arg, 0, sizeof(arg));
        put64((uint8_t*)&bin + 0x00, 0x1c000);
        spu_workload_reset();
        CHECK(spu_workload_register_direct(
                  spu_workload_fingerprint(one_shot_elf, 0x104), 0x104,
                  synthetic_exit_task, "synthetic-exit-task"));
        one_shot_runs = 0;
        for (uint32_t iteration = 0; iteration < 260; ++iteration) {
            CHECK(cellSpursCreateTask2WithBinInfo(ts, id, &bin, &arg,
                                                 NULL, "one-shot", NULL) == 0);
            CHECK(be32(id) == 0);
            CHECK(cellSpursJoinTask2(ts, be32(id), exit_code) == 0);
            CHECK(be32(exit_code) == iteration + 1);
            CHECK((ts->bytes[0x00] & 0x80) == 0);
            CHECK((ts->bytes[0x10] & 0x80) == 0);
            CHECK((ts->bytes[0x30] & 0x80) == 0);
            CHECK((ts->bytes[0x40] & 0x80) == 0);
            CHECK((ts->bytes[0x50] & 0x80) == 0);
        }
        CHECK(one_shot_runs == 260);
    }

    CHECK(_cellSpursEventFlagInitialize(NULL, (CellSpursTaskset*)ts, ef,
                                        CELL_SPURS_EVENT_FLAG_CLEAR_AUTO,
                                        CELL_SPURS_EVENT_FLAG_ANY2ANY) == 0);
    put16(bits, 0x0001);
    CHECK(cellSpursEventFlagWait(ef, bits, CELL_SPURS_EVENT_FLAG_OR) ==
          CELL_SPURS_TASK_ERROR_STAT);
    CHECK(cellSpursEventFlagAttachLv2EventQueue(ef) == 0);
    CHECK(cellSpursEventFlagSet(ef, 0x00a0) == 0);
    put16(bits, 0x0020);
    CHECK(cellSpursEventFlagWait(ef, bits, CELL_SPURS_EVENT_FLAG_OR) == 0);
    CHECK((guest[0x9010] == 0 && guest[0x9011] == 0x20));
    CHECK((ef->bytes[0] == 0 && ef->bytes[1] == 0x80));
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        EventWaitArgs args = {ef, bits, -1};
        put16(bits, 0x0040);
#if defined(_WIN32)
        HANDLE thread = CreateThread(NULL, 0, event_wait_thread, &args, 0, NULL);
        CHECK(thread != NULL);
        CHECK(cellSpursEventFlagSet(ef, 0x0040) == 0);
        CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(thread);
#else
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, event_wait_thread, &args) == 0);
        CHECK(cellSpursEventFlagSet(ef, 0x0040) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(args.rc == 0);
        CHECK(guest[0x9010] == 0 && guest[0x9011] == 0x40);
    }
    {
        EventWaitArgs args = {ef, bits, -1};
        put16(bits, 0x0001);
#if defined(_WIN32)
        HANDLE thread = CreateThread(NULL, 0, event_wait_thread, &args, 0, NULL);
        CHECK(thread != NULL);
#else
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, event_wait_thread, &args) == 0);
#endif
        for (unsigned spin = 0; spin < 1000 && be16(ef->bytes + 0x04) != 0x0001; ++spin) {
#if defined(_WIN32)
            Sleep(1);
#else
            sched_yield();
#endif
        }
        CHECK(be16(ef->bytes + 0x04) == 0x0001);
        const uint32_t slot = ef->bytes[0x06] >> 4;
        const uint16_t slot_bit = (uint16_t)(0x8000u >> slot);
        /* Q2-B: a PPU waiter records its slot ONLY in ppuWaitSlotAndMode
         * [0x06]; spuTaskUsedWaitSlots[0x08] stays clear (rpcs3 oracle,
         * cellSpurs.cpp:3552-3567). The pre-sweep assertion expected the
         * phantom bit this fix removed. */
        CHECK((be16(ef->bytes + 0x08) & slot_bit) == 0);
        put16(ef->bytes + 0x30 + slot * 2, 0x0001);
        put16(ef->bytes + 0x04, 0);
        ef->bytes[0x07] = 1;
        cellSpursNotifyGuestWrite(0x6800, 128);
#if defined(_WIN32)
        CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(thread);
#else
        CHECK(pthread_join(thread, NULL) == 0);
#endif
        CHECK(args.rc == 0);
        CHECK(be16(bits) == 0x0001);
        CHECK(ef->bytes[0x07] == 0);
        CHECK((be16(ef->bytes + 0x08) & slot_bit) == 0);
    }
    CHECK(cellSpursEventFlagDetachLv2EventQueue(ef) == 0);

    CHECK(_cellSpursQueueInitialize(NULL, (CellSpursTaskset*)ts, push_queue,
                                    push_data, 16, 2, 2) == 0);
    {
        _Alignas(16) uint32_t a[4] = {
            0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00
        };
        _Alignas(16) uint32_t b[4] = {0};
        CHECK(cellSpursQueuePushBody(push_queue, a, 1) == 0);
        CHECK(be32(push_queue->bytes + 0x04) == 1);
        CHECK(!memcmp(push_data, a, sizeof(a)));

        CHECK(_cellSpursQueueInitialize(NULL, (CellSpursTaskset*)ts, pop_queue,
                                        pop_data, 16, 2, 1) == 0);
        memcpy(pop_data, a, sizeof(a));
        put32(pop_queue->bytes + 0x04, 1); /* completed SPU producer */
        cellSpursNotifyGuestWrite(0x6a00, 128);
        CHECK(cellSpursQueuePopBody(pop_queue, b, 0, 1) == 0);
        CHECK(!memcmp(b, a, sizeof(a)));
        CHECK(be32(pop_queue->bytes + 0x00) == 1);

        CHECK(_cellSpursLFQueueInitialize(ts, lf_queue, lf_data,
                                          16, 2, 2) == 0);
        CHECK(be32(lf_queue->bytes + 0x10) == 16);
        CHECK(be32(lf_queue->bytes + 0x14) == 2);
        CHECK(cellSpursQueuePushBody(push_queue, a, 0) == 0);
        CHECK(_cellSpursLFQueuePushBody(lf_queue, a, 1) == 0);
        CHECK(lf_queue->bytes[0x08] == 0 && lf_queue->bytes[0x09] == 1);
        CHECK(!memcmp(lf_data, a, sizeof(a)));

        /* The SPU blocking-pop contract publishes a 16-bit workload/task
         * token in the consumer wait list before the task enters WAITING.
         * One PPU push must wake one such task after publishing its payload. */
        CHECK(spu_workload_register_direct(
                  spu_workload_fingerprint(elf, 0x104), 0x104,
                  synthetic_task, "synthetic-lfqueue-wait-task"));
        CHECK(_cellSpursLFQueueInitialize(ts, lf_wait_queue, lf_wait_data,
                                          16, 1, 2) == 0);
        task_phase = 0;
        task_abi_ok = 0;
        {
            CellSpursTaskBinInfo bin;
            CellSpursTaskArgument arg;
            memset(&bin, 0, sizeof(bin));
            memset(&arg, 0, sizeof(arg));
            put64((uint8_t*)&bin + 0x00, 0x8000);
            put32((uint8_t*)&bin + 0x08, 1024);
            put32(arg.bytes + 0x00, 0x01020304);
            put32(arg.bytes + 0x04, 0x11121314);
            put32(arg.bytes + 0x08, 0x21222324);
            put32(arg.bytes + 0x0c, 0x31323334);
            CHECK(cellSpursCreateTask2WithBinInfo(
                      ts, id, &bin, &arg, task_context + 0x800,
                      "lfqueue-wait-task", NULL) == 0);
        }
        CHECK(wait_phase(1));
        CHECK(task_abi_ok);
        CHECK(wait_byte_mask(ts->bytes + 0x50, 0x80, 0x80));
        put16(lf_wait_queue->bytes + 0x30, 1);
        put16(lf_wait_queue->bytes + 0x32,
              (uint16_t)((6u << 8) | be32(id)));
        cellSpursNotifyGuestWrite(0x7300, 128);
        CHECK(_cellSpursLFQueuePushBody(lf_wait_queue, a, 1) == 0);
        CHECK(cellSpursJoinTask2(ts, be32(id), exit_code) == 0);
        CHECK(task_phase == 2);
        CHECK(be32(exit_code) == 0x2468);
        CHECK(!memcmp(lf_wait_data, a, sizeof(a)));
    }
    CHECK(cellSpursShutdownTaskset((CellSpursTaskset*)ts) == 0);
    CHECK(cellSpursJoinTaskset((CellSpursTaskset*)ts) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    (void)priority;
    return 0;
}

static int test_job_chain(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0xa000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0xb000);
    CellSpursJobChainAttribute* ja = (CellSpursJobChainAttribute*)(guest + 0xb800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0xc000);
    uint64_t* commands = (uint64_t*)(guest + 0xd000);
    CellSpursJobGuard* guard = (CellSpursJobGuard*)(guest + 0xd080);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};
    memset(binary, 0x5a, 16);
    memset(descriptor, 0, 0x40);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);  /* write back input/output DMA list */
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands + 0, 0x000000000000d003ull); /* circular empty NEXT */
    put64(commands + 1, 0);                     /* consumed without parking */
    put64(commands + 2, 0);                     /* consumed without parking */
    put64(commands + 3, 0x000000000000d01bull); /* third circular empty */
    put64(commands + 4, 0);                     /* batch body */
    put64(commands + 5, 0x000000000000d08full); /* GUARD */
    put64(commands + 6, 0x000000000000d01bull); /* wrap to third slot */
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job"));
    {
        uint8_t unknown[16];
        spu_workload_image image;
        memset(unknown, 0xa5, sizeof(unknown));
        CHECK(!spu_workload_resolve(unknown, sizeof(unknown), &image));
    }
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(3, 0x475001, ja, commands,
                                                0x30, 1, priority, 1, 0,
                                                 0, 1, 0, 0x100, 1) ==
          CELL_SPURS_JOB_ERROR_INVAL);
    CHECK(_cellSpursJobChainAttributeInitialize(3, 0x475001, ja, commands,
                                                0x40, 1, priority, 1, 0,
                                                 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    CHECK(cellSpursJobGuardInitialize(jc, guard, 1, 1, 1) == 0);
    CHECK(cellSpursJobGuardNotify(guard) == 0);
    CHECK(be32(guard->bytes) == 0);
    CHECK(cellSpursJobGuardNotify(guard) == CELL_SPURS_JOB_ERROR_STAT);
    CHECK(cellSpursJobGuardReset(guard) == 0);
    CHECK(be32(guard->bytes) == 1);
    put32(guard->bytes, 0);
    cellSpursNotifyGuestWrite(0xd080, 4);
    CHECK(cellSpursJobGuardNotify(guard) == CELL_SPURS_JOB_ERROR_STAT);
    CHECK(cellSpursJobGuardReset(guard) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_dma_tag_seen_mask = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);

    /* Move through two NOP slots and park on the third.  The producer then
     * clears and refills a consumed slot on which the consumer never parked.
     * That wrapped head must become the resume point.  Notify only the final
     * low word to cover the title's two-store 64-bit publication sequence. */
    put64(commands + 0, 0x000000000000d00bull);
    cellSpursNotifyPpuGuestWrite(0xd004, 4);
    brief_sleep();
    put64(commands + 2, 0x0000000800000012ull);
    cellSpursNotifyGuestWrite(0xd010, 8);
    job_hold = 1;
    put64(commands + 2, 0xe000);
    cellSpursNotifyGuestWrite(0xd010, 8);
    CHECK(wait_value(&job_runs, 1));
    CHECK(wait_value(&job_entered, 1));
    /* Refill another consumed slot while the first wrapped job is still
     * executing.  The current generation must finish before this deferred
     * resume point is applied at the next empty boundary. */
    put64(commands + 1, 0x0000000800000012ull);
    cellSpursNotifyGuestWrite(0xd008, 8);
    put64(commands + 1, 0xe000);
    cellSpursNotifyGuestWrite(0xd008, 8);
    put64(commands + 2, 0x000000000000d01bull);
    cellSpursNotifyGuestWrite(0xd010, 8);
    job_hold = 0;
    CHECK(wait_value(&job_runs, 2));
    brief_sleep();

    put64(commands + 3, 0xe000);
    put64(commands + 4, 0xe000);
    /* The live command producer is an SPU task.  A completed DMA publication
     * must wake the same parked chain slot as a lifted PPU store.  Publication
     * claims the filled body before the producer can reuse a consumed slot. */
    cellSpursNotifyGuestWrite(0xd018, 8);
    put64(commands + 4, 0); /* reuse after the published view was claimed */
    CHECK(wait_value(&job_runs, 4));
    put64(commands + 3, 0x000000000000d01bull);
    cellSpursNotifyPpuGuestWrite(0xd018, 8);
    put32(guard->bytes, 0);
    cellSpursNotifyGuestWrite(0xd080, 4);
    CHECK(wait_be32_value(guard->bytes, 1));
    put64(commands + 3, 0x0000000800000012ull);
    cellSpursNotifyPpuGuestWrite(0xd018, 8);
    CHECK(cellSpursShutdownJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 4);
    CHECK(job_abi_ok);
    CHECK(job_dma_tag_seen_mask == 3u);
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(inout[i] == (uint8_t)i);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_unnotified_pending_ticket_orbit(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x58000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x59000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x59800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x5a000);
    uint64_t* commands = (uint64_t*)(guest + 0x5a800);
    /* Deliberately only 16-byte aligned. CellSpursJob does not require the
     * 128-byte alignment that the old bridge-address reconstruction assumed. */
    uint8_t* descriptor = guest + 0x5b030;
    uint8_t* binary = guest + 0x5c000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x6b, 16);
    put64(descriptor, 0x5c000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 0);
    put32(descriptor + 0x10, 0);
    put32(descriptor + 0x14, 0);
    put64(commands + 0, 0x0000000800000012ull);
    put64(commands + 1, 0x0000000800000012ull);
    put64(commands + 2, 0x0000000800000012ull);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_ticket_job, "synthetic-ticket-job"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority, 1, 0,
              0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);

    /* The real producer publishes this reusable descriptor in two stores.
     * The counter edge alone is incomplete; the following ready-word store
     * is the first point at which redispatch is valid. */
    {
        uint32_t candidate_descriptor = 0;
        CHECK(yz_spurs_test_watch_pending_descriptor(jc, 0x5b030) == 0);
        put32(descriptor + 0x10, 6);
        CHECK(yz_spurs_test_pending_ticket_publish_candidate(
                  jc, 0x5b040, 4, &candidate_descriptor) == 0);
        put32(descriptor + 0x14, 16);
        CHECK(yz_spurs_test_pending_ticket_publish_candidate(
                  jc, 0x5b044, 4, &candidate_descriptor) == 1);
        CHECK(candidate_descriptor == 0x5b030);
        put32(descriptor + 0x10, 0);
    }
    ticket_job_runs = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    {
        int parked = 0;
        for (unsigned i = 0; i < 1000000 && !parked; ++i) {
            parked = yz_spurs_jobchain_is_parked_at(jc, 0x5a800);
            if (!parked)
#if defined(_WIN32)
                SwitchToThread();
#else
                sched_yield();
#endif
        }
        CHECK(parked);
    }

    /* A narrow producer store can transiently combine the old empty-marker
     * upper word with the new JOB lower word.  That mixed 64-bit generation
     * is not a descriptor command and must remain parked until the producer
     * publishes the reserved-zero upper word (Frontier acceptance run 4 saw
     * 0x00000008_001ACB80 captured ahead of live 0x00000000_401ACB80). */
    put64(commands + 0, 0x000000080005b030ull);
    cellSpursNotifyPpuGuestWrite(0x5a804, 4);
    brief_sleep();
    CHECK(ticket_job_runs == 0);

    put64(commands + 0, 0x5b030);
    cellSpursNotifyGuestWrite(0x5a800, 8);
    CHECK(wait_value(&ticket_job_runs, 1));
    {
        int parked = 0;
        for (unsigned i = 0; i < 1000000 && !parked; ++i) {
            parked = yz_spurs_jobchain_is_parked_at(jc, 0x5a808);
            if (!parked)
#if defined(_WIN32)
                SwitchToThread();
#else
                sched_yield();
#endif
        }
        CHECK(parked);
    }

    /* Force the live command to be recycled in the narrow interval between
     * bridge selection and claim.  This used to make the fallback copy an
     * unrelated stack buffer into the JobCommandSnapshot. */
    pending_ticket_claim_hook_slot = 0x5a800;
    pending_ticket_claim_hook_descriptor = 0x5b030;
    atomic_store_explicit(
        &pending_ticket_claim_hook_armed, 1, memory_order_release);
    /* The live producer exposes kind/ready before finishing the descriptor
     * body. A parked poll used to acquire this stable partial generation and
     * terminate the chain with INVALID_BIN. The PPU store now stages the
     * candidate until the producer's publication fence. */
    put64(descriptor + 0x00, 0);
    put16(descriptor + 0x08, 0);
    put32(descriptor + 0x10, 6);
    put32(descriptor + 0x14, 16);
    cellSpursNotifyPpuGuestWrite(0x5b044, 4);
    brief_sleep();
    CHECK(ticket_job_runs == 1);
    CHECK(be32(descriptor + 0x10) == 6);
    put64(descriptor + 0x00, 0x5c000);
    put16(descriptor + 0x08, 1);
    cellSpursNotifyPpuFence();
    CHECK(wait_value(&ticket_job_runs, 2));
    CHECK(wait_be32_value(descriptor + 0x10, 0));
    CHECK(atomic_load_explicit(
              &pending_ticket_claim_hook_armed, memory_order_acquire) == 0);

    /* Acquire the same persistent dispatcher through a newer ring slot.  A
     * retained descriptor map now has two historical owners; only this latest
     * acquisition carries the continuation context for the active orbit. */
    put64(commands + 1, 0x5b030);
    cellSpursNotifyGuestWrite(0x5a808, 8);
    CHECK(wait_value(&ticket_job_runs, 3));

    /* Once the persistent dispatcher JOB has been acquired, the producer may
     * recycle its command-ring slot before publishing the next descriptor
     * ticket.  Redispatch must use the most recently acquired command, not
     * rediscover the descriptor from mutable memory or pick its older slot. */
    put64(commands + 0, 0x0000000800000012ull);
    put64(commands + 1, 0x0000000800000012ull);
    put32(descriptor + 0x10, 6);
    put32(descriptor + 0x14, 16);
    cellSpursNotifyPpuGuestWrite(0x5b044, 4);
    cellSpursNotifyPpuFence();
    CHECK(wait_value(&ticket_job_runs, 4));
    CHECK(wait_be32_value(descriptor + 0x10, 0));
    {
        int parked = 0;
        for (unsigned i = 0; i < 1000000 && !parked; ++i) {
            parked = yz_spurs_jobchain_is_parked_at(jc, 0x5a810);
            if (!parked)
#if defined(_WIN32)
                SwitchToThread();
#else
                sched_yield();
#endif
        }
        CHECK(parked);
    }

    /* Without a write notification there is no sound way to associate this
     * storage with the publishing PPU thread. In particular, a timing-based
     * parked poll must not execute it before a barrier. Once the store edge
     * and fence are supplied, the persistent orbit resumes. */
    put64(commands + 1, 0x5b030);
    put32(descriptor + 0x10, 6);
    brief_sleep();
    CHECK(ticket_job_runs == 4);
    CHECK(be32(descriptor + 0x10) == 6);
    cellSpursNotifyPpuGuestWrite(0x5b040, 4);
    cellSpursNotifyPpuFence();
    CHECK(wait_value(&ticket_job_runs, 5));
    CHECK(wait_be32_value(descriptor + 0x10, 0));

    /* The live title can publish the next ticket while the persistent
     * dispatcher is still executing the previous orbit.  The PPU-store edge
     * must be retained as a deferred resume and consumed at the next park;
     * a parked-only fallback cannot cover this ordering. */
    ticket_job_hold = 1;
    ticket_job_entered = 0;
    put32(descriptor + 0x10, 6);
    put32(descriptor + 0x14, 16);
    cellSpursNotifyPpuGuestWrite(0x5b044, 4);
    cellSpursNotifyPpuFence();
    CHECK(wait_value(&ticket_job_runs, 6));
    CHECK(wait_value(&ticket_job_entered, 1));
    CHECK(wait_be32_value(descriptor + 0x10, 0));
    put32(descriptor + 0x10, 6);
    put32(descriptor + 0x14, 16);
    cellSpursNotifyPpuGuestWrite(0x5b044, 4);
    cellSpursNotifyPpuFence();
    ticket_job_hold = 0;
    CHECK(wait_value(&ticket_job_runs, 7));
    CHECK(wait_be32_value(descriptor + 0x10, 0));

    /* Frontier reuses both arenas after this persistent command has parked:
     * its old command slot becomes DDS payload and its old descriptor becomes
     * an unrelated pointer table.  The ordinary data can coincidentally have
     * kind=6 and a nonzero ready word.  A retained address alone must not turn
     * that recycled storage back into a JOB (acceptance run 3 reproduced this
     * with command 0x40000080 and descriptor 0x40004080). */
    put64(commands + 0, 0x0000000800000012ull);
    put64(commands + 1, 0x0000000800000012ull);
    put64(descriptor + 0x00, 0x0000000260814d00ull);
    put16(descriptor + 0x08, 3);
    put32(descriptor + 0x14, 0x60814f00);
    descriptor[0x2c] = 0x60;
    put32(descriptor + 0x10, 6); /* publish the accidental lookalike last */
    for (unsigned i = 0; i < 10; ++i)
        brief_sleep();
    CHECK(ticket_job_runs == 7);
    CHECK(be32(descriptor + 0x10) == 6);

    CHECK(cellSpursShutdownJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

typedef struct LockOrderPushArgs {
    CellSpursLFQueue* queue;
    const void* data;
    int32_t rc;
    _Atomic int done;
} LockOrderPushArgs;

typedef struct LockOrderNotifyArgs {
    uint32_t ea;
    _Atomic int done;
} LockOrderNotifyArgs;

#if defined(_WIN32)
static DWORD WINAPI lock_order_push_thread(LPVOID raw)
#else
static void* lock_order_push_thread(void* raw)
#endif
{
    LockOrderPushArgs* args = (LockOrderPushArgs*)raw;
    args->rc = _cellSpursLFQueuePushBody(args->queue, args->data, 1);
    atomic_store_explicit(&args->done, 1, memory_order_release);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI lock_order_notify_thread(LPVOID raw)
#else
static void* lock_order_notify_thread(void* raw)
#endif
{
    LockOrderNotifyArgs* args = (LockOrderNotifyArgs*)raw;
    spu_lockline_unlock_then_notify_guest_write(1, args->ea, 128);
    atomic_store_explicit(&args->done, 1, memory_order_release);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int test_spurs_queue_lock_order(void)
{
    CellSpursTaskset* owner = (CellSpursTaskset*)(guest + 0x13000);
    CellSpursLFQueue* queue = (CellSpursLFQueue*)(guest + 0x12000);
    uint8_t* queue_data = guest + 0x12100;
    _Alignas(16) uint32_t item[4] = {
        0x10203040u, 0x50607080u, 0x90a0b0c0u, 0xd0e0f000u
    };
    LockOrderPushArgs push = {queue, item, -1, 0};
    LockOrderNotifyArgs notify = {0x12000u, 0};

    memset(owner, 0, sizeof(*owner));
    memset(queue, 0, sizeof(*queue));
    memset(queue_data, 0, 16);
    CHECK(_cellSpursLFQueueInitialize(
              owner, queue, queue_data, 16, 1, 2) == 0);

    /* Force the historical AB-BA interleaving:
     *   push thread: queue mutex -> waits for lockline
     *   writer:      lockline -> queue notification
     * The production helper must release the lockline before entering SPURS,
     * allowing both threads to make bounded forward progress. */
    atomic_store_explicit(&lock_order_hook_armed, 1, memory_order_release);
    atomic_store_explicit(
        &lock_order_queue_mutex_held, 0, memory_order_release);
    atomic_store_explicit(
        &lock_order_queue_mutex_release, 0, memory_order_release);
    spu_lockline_lock();

#if defined(_WIN32)
    HANDLE push_thread = CreateThread(
        NULL, 0, lock_order_push_thread, &push, 0, NULL);
    CHECK(push_thread != NULL);
#else
    pthread_t push_thread;
    CHECK(pthread_create(
              &push_thread, NULL, lock_order_push_thread, &push) == 0);
#endif
    CHECK(wait_atomic_value(&lock_order_queue_mutex_held, 1));

#if defined(_WIN32)
    HANDLE notify_thread = CreateThread(
        NULL, 0, lock_order_notify_thread, &notify, 0, NULL);
    CHECK(notify_thread != NULL);
#else
    pthread_t notify_thread;
    CHECK(pthread_create(
              &notify_thread, NULL, lock_order_notify_thread, &notify) == 0);
#endif
    atomic_store_explicit(
        &lock_order_queue_mutex_release, 1, memory_order_release);

    CHECK(wait_atomic_value(&push.done, 1));
    CHECK(wait_atomic_value(&notify.done, 1));
#if defined(_WIN32)
    CHECK(WaitForSingleObject(push_thread, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(notify_thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(push_thread);
    CloseHandle(notify_thread);
#else
    CHECK(pthread_join(push_thread, NULL) == 0);
    CHECK(pthread_join(notify_thread, NULL) == 0);
#endif
    atomic_store_explicit(&lock_order_hook_armed, 0, memory_order_release);
    CHECK(push.rc == 0);
    CHECK(!memcmp(queue_data, item, sizeof(item)));
    return 0;
}

static int test_job_logical_binary_size(void)
{
    uint8_t binary[0x50];
    spu_workload_image image;

    memset(binary, 0x5a, 0x40);
    memset(binary + 0x40, 0, 0x10);
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 0x40), 0x40,
              synthetic_job, "logical-job-binary"));
    CHECK(spu_workload_resolve(binary, 0x40, &image));

    /* An aligned DMA/extraction span may include bytes beyond sizeBinary.
     * Exact native dispatch must use the descriptor's logical length. */
    CHECK(!spu_workload_resolve(binary, 0x50, &image));
    return 0;
}

static int test_job_sub_qword_dma_layout(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x68000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x69000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x69800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x6a000);
    uint64_t* commands = (uint64_t*)(guest + 0x6d000);
    uint8_t* descriptor = guest + 0x6e000;
    uint8_t* binary = guest + 0x6f000;
    uint8_t* data = guest + 0x70000;
    uint8_t expected[0x80];
    static const uint32_t data_offset[4] = {4u, 0x28u, 0x4cu, 0x70u};
    static const uint32_t input[4] = {
        0x10203040u, 0x50607080u, 0x90a0b0c0u, 0xd0e0f000u
    };
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x80);
    memset(binary, 0x6b, 16);
    memset(data, 0xa5, 0x80);
    for (uint32_t i = 0; i < 4; ++i)
        put32(data + data_offset[i], input[i]);
    memcpy(expected, data, sizeof(expected));
    for (uint32_t i = 0; i < 4; ++i)
        put32(expected + data_offset[i], input[i] ^ 0xffffffffu);

    put64(descriptor, 0x6f000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 40);
    put32(descriptor + 0x10, 1);  /* publish the four in/out elements */
    put32(descriptor + 0x14, 64); /* one 16-byte LS slot per element */
    for (uint32_t i = 0; i < 4; ++i)
        put64(descriptor + 0x30 + i * 8,
              ((uint64_t)4 << 32) | (0x70000u + data_offset[i]));
    /* MFC list bit 31 is stall-and-notify.  A zero-size final element is a
     * valid title-observed sentinel; it transfers no bytes and consumes no
     * IO slot, but remains part of the descriptor's list count. */
    put64(descriptor + 0x50, 0x8000000000000001ull);
    put64(commands + 0, 0x6e000);
    put64(commands + 1, 0x7f);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_sub_qword_job, "synthetic-job-sub-qword-dma"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x80, 1, priority, 1, 0,
              1, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    sub_qword_job_runs = 0;
    sub_qword_layout_ok = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(sub_qword_job_runs == 1);
    CHECK(sub_qword_layout_ok);
    CHECK(!memcmp(data, expected, sizeof(expected)));
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_large_job_slot_fallback(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x74000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x75000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x75800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x76000);
    uint64_t* commands = (uint64_t*)(guest + 0x77000);
    uint8_t* warmup_desc = guest + 0x78000;
    uint8_t* large_desc = guest + 0x78080;
    uint8_t* warmup_binary = guest + 0x79000;
    uint8_t* large_binary = guest + 0x80000;
    uint8_t* input = guest + 0x92000;
    uint8_t* cache = guest + 0x93000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(warmup_desc, 0, 0x80);
    memset(large_desc, 0, 0x80);
    memset(warmup_binary, 0x6c, 16);
    memset(large_binary, 0x7d, 0x11280);
    memset(input, 0x2a, 0x40);
    memset(cache, 0x3b, 0x1000);

    put64(warmup_desc, 0x79000);
    put16(warmup_desc + 8, 1);

    put64(large_desc, 0x80000);
    put16(large_desc + 8, 0x1128); /* 0x11280-byte job binary */
    put16(large_desc + 0x0a, 8);
    put32(large_desc + 0x10, 0x13579bdf);
    put32(large_desc + 0x14, 0x8000);
    put16(large_desc + 0x1c, 0x0800); /* 0x8000-byte stack */
    put16(large_desc + 0x1e, 0x1800); /* 0x18000-byte scratch */
    put32(large_desc + 0x24, 8);
    put64(large_desc + 0x30, ((uint64_t)0x40 << 32) | 0x92000u);
    put64(large_desc + 0x38, ((uint64_t)0x1000 << 32) | 0x93000u);

    /* The warmup consumes the initial 0x4C00 preference. The large job is
     * then offered 0xE400, where its complete layout cannot fit.  At 0x4C00
     * the allocation ends exactly at the descriptor boundary (0x3F000),
     * matching Frontier's fifth Job E descriptor. */
    put64(commands + 0, 0x78000);
    put64(commands + 1, 0x78080);
    put64(commands + 2, 0x7f);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(warmup_binary, 16), 16,
              synthetic_layout_warmup_job, "layout-warmup-job"));
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(large_binary, 0x11280), 0x11280,
              synthetic_large_layout_job, "large-layout-job"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x80, 1, priority,
              1, 0, 0, 1, 0, 0x200, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    large_job_runs = 0;
    large_job_slot = 0;
    large_job_layout_ok = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(large_job_runs == 1);
    CHECK(large_job_slot == 0x04c00u);
    CHECK(large_job_layout_ok);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_job_descriptor_snapshot(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x42000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x43000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x43800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x44000);
    uint64_t* commands = (uint64_t*)(guest + 0x45000);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t* decoy = guest + 0x10100;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    memset(decoy, 0xa5, 16);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands + 0, 0xe000);
    put64(commands + 1, 0x7f);
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-descriptor-snapshot"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority, 1, 7,
              8, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 1;
    job_entered = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(wait_value(&job_entered, 1));

    /* A producer may reuse guest descriptor storage after the worker has
     * grabbed it.  Completion must retain the grabbed output contract. */
    put32(descriptor + 0x10, 0);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10100);
    job_hold = 0;
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 1);
    CHECK(job_abi_ok);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(inout[i] == (uint8_t)(i ^ 0xff));
        CHECK(decoy[i] == 0xa5);
    }
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_job_descriptor_stable_acquisition(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x5a000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x5b000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x5b800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x5c000);
    uint64_t* commands = (uint64_t*)(guest + 0x5d000);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* original = guest + 0x10000;
    uint8_t* replacement = guest + 0x10100;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    for (uint32_t i = 0; i < 16; ++i) {
        original[i] = (uint8_t)i;
        replacement[i] = (uint8_t)(0x40u + i);
    }
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands + 0, 0xe000);
    put64(commands + 1, 0x7f);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-stable-acquisition"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority,
              1, 0, 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);

    descriptor_snapshot_hook_ea = 0xe000;
    descriptor_snapshot_replacement_ea = 0x10100;
    atomic_store_explicit(
        &descriptor_snapshot_hook_count, 0, memory_order_release);
    atomic_store_explicit(
        &descriptor_snapshot_hook_armed, 1, memory_order_release);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(atomic_load_explicit(
              &descriptor_snapshot_hook_count, memory_order_acquire) == 1);
    CHECK(job_runs == 1);
    CHECK(job_abi_ok);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(original[i] == (uint8_t)i);
        CHECK(replacement[i] == (uint8_t)((0x40u + i) ^ 0xffu));
    }

    /* A completed chain can be run again.  The title-observed lifecycle starts
     * the second run at jobChainEntry and discards the prior END/current/ring
     * publication state. */
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 2);
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(replacement[i] == (uint8_t)(0x40u + i));
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_job_barrier_snapshot(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x20000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x21000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x21800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x22000);
    CellSpursJobChainAttribute* claimed_ja =
        (CellSpursJobChainAttribute*)(guest + 0x21900);
    CellSpursJobChain* claimed_jc =
        (CellSpursJobChain*)(guest + 0x22800);
    uint64_t* commands = (uint64_t*)(guest + 0x23000);
    uint64_t* claimed_commands = (uint64_t*)(guest + 0x24000);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t* decoy = guest + 0x10100;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    memset(decoy, 0xa5, 16);
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);

    /* Hold the first job while the producer publishes a SYNC plus a second
     * job at the next command.  Recycle the live body immediately: the
     * worker must consume the immutable publication on its ordinary path. */
    put64(commands + 0, 0xe000);
    put64(commands + 1, 0x000000000002300bull); /* empty self NEXT */
    put64(commands + 2, 0);
    put64(commands + 3, 0x7f);                  /* END */
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-barrier"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(3, 0x475001, ja, commands,
                                                0x40, 1, priority, 1, 0,
                                                0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 1;
    job_entered = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(wait_value(&job_entered, 1));
    put64(commands + 1, 0x000000000000000aull); /* SYNC_LABEL */
    put64(commands + 2, 0xe000);
    cellSpursNotifyGuestWrite(0x23008, 8);
    put64(commands + 2, 0); /* reuse after the view was claimed */
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10100);
    job_hold = 0;
    CHECK(wait_value(&job_runs, 2));
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 2);
    CHECK(job_abi_ok);
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(inout[i] == (uint8_t)i);
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(decoy[i] == 0xa5);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);

    /* An exact SYNC write at the slot where the worker is already parked is
     * claimed immediately.  It must not also remain in the future-publication
     * queue and replay when this generation wraps back to the same slot. */
    memset(claimed_jc, 0, sizeof(*claimed_jc));
    put64(claimed_commands + 0, 0);                       /* NOP */
    put64(claimed_commands + 1, 0x000000000002400bull);  /* self NEXT */
    put64(claimed_commands + 2, 0);
    put64(claimed_commands + 3, 0x0000000000024009ull);  /* RESET_PC +1 */
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, claimed_ja, claimed_commands, 0x40, 1,
              priority, 1, 0, 0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(
              spurs, claimed_jc, claimed_ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 0;
    CHECK(cellSpursRunJobChain(claimed_jc) == 0);
    /* Deterministic parked edge (2026-08-05): the exact-SYNC-at-parked-slot
     * claim is only exercised when the worker is ALREADY parked at 0x24008.
     * The brief_sleep formerly here lost that race ~2-3/12 runs -- the SYNC
     * then arrived as a future publication and replayed on generation wrap,
     * failing the job_runs == 1 check below (the known flake). */
    {
        int parked = 0;
        for (unsigned i = 0; i < 1000000 && !parked; ++i) {
            parked = yz_spurs_jobchain_is_parked_at(claimed_jc, 0x24008);
            if (!parked)
#if defined(_WIN32)
                SwitchToThread();
#else
                sched_yield();
#endif
        }
        CHECK(parked);
    }
    put64(claimed_commands + 1, 0x0000000000000002ull); /* SYNC */
    put64(claimed_commands + 2, 0xe000);
    cellSpursNotifyGuestWrite(0x24008, 8);
    put64(claimed_commands + 1, 0x000000000002400bull);
    put64(claimed_commands + 2, 0);
    CHECK(wait_value(&job_runs, 1));
    brief_sleep();
    CHECK(job_runs == 1);
    CHECK(job_abi_ok);
    CHECK(cellSpursShutdownJobChain(claimed_jc) == 0);
    CHECK(cellSpursJoinJobChain(claimed_jc) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_incomplete_barrier_descriptor_publication(void)
{
    /* Reuse the barrier test's SPURS key after finalization. */
    CellSpurs* spurs = (CellSpurs*)(guest + 0x20000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x21000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x94800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x95000);
    uint64_t* commands = (uint64_t*)(guest + 0x95800);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor + 0x00, 0xf000);
    put16(descriptor + 0x08, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands + 0, 0xe000);
    put64(commands + 1, 0x000000000009580bull); /* empty self NEXT */
    put64(commands + 2, 0);
    put64(commands + 3, 0x7f);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-incomplete-publication"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority,
              1, 0, 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 1;
    job_entered = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(wait_value(&job_entered, 1));

    /* Acceptance run 19: a SYNC claim observed a stable all-zero copy of a
     * later descriptor. The producer completed that same descriptor before
     * the worker reached it, but the old immutable zero copy killed the chain
     * with INVALID_BIN. A claim must stop before an unpublished JOB and let
     * the later live fetch acquire the completed generation. */
    put64(descriptor + 0x00, 0);
    put16(descriptor + 0x08, 0);
    put64(commands + 1, 0x02ull);
    put64(commands + 2, 0xe000);
    cellSpursNotifyGuestWrite(0x95808, 8);
    put64(descriptor + 0x00, 0xf000);
    put16(descriptor + 0x08, 1);
    job_hold = 0;
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 2);
    CHECK(job_abi_ok);
    CHECK(yz_spurs_test_pending_snapshot_count(jc) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_call_tail_late_publication(void)
{
    /* Reuse the barrier test's registered SPURS key: the harness deliberately
     * keeps finalized instance slots for same-key reinitialization. */
    CellSpurs* spurs = (CellSpurs*)(guest + 0x20000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x21000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x61800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x62000);
    uint64_t* commands = (uint64_t*)(guest + 0x63000);
    uint64_t* callee = (uint64_t*)(guest + 0x64000);
    uint8_t* descriptor = guest + 0x65000;
    uint8_t* binary = guest + 0x66000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(commands, 0, 3 * sizeof(*commands));
    memset(callee, 0, 2 * sizeof(*callee));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x6c, 16);
    put64(descriptor + 0x00, 0x66000);
    put16(descriptor + 0x08, 1);
    put64(commands + 0, 0x0000000000063003ull); /* empty self NEXT */
    put64(commands + 2, 0x7full);                /* END after CALL returns */
    put64(callee + 0, 0x65000);                  /* body published first */
    put64(callee + 1, 0);                        /* RET not published yet */

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_call_tail_job, "synthetic-call-tail-job"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority,
              1, 0, 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    call_tail_job_runs = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    {
        int parked = 0;
        for (unsigned i = 0; i < 1000000 && !parked; ++i) {
            parked = yz_spurs_jobchain_is_parked_at(jc, 0x63000);
            if (!parked)
#if defined(_WIN32)
                SwitchToThread();
#else
                sched_yield();
#endif
        }
        CHECK(parked);
    }

    /* Publish SYNC last.  Its immutable view follows CALL into the callee,
     * where the test hook supplies RET only after the worker first observes
     * the zero tail.  The chain must return to commands[2], not preserve the
     * zero and walk into arbitrary guest memory. */
    put64(commands + 1, 0x0000000000064004ull); /* CALL 0x64000 */
    call_tail_publication_hook_ea = 0x64008;
    atomic_store_explicit(
        &call_tail_publication_hook_armed, 1, memory_order_release);
    put64(commands + 0, 0x02ull);               /* SYNC publication head */
    cellSpursNotifyGuestWrite(0x63000, 8);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(call_tail_job_runs == 1);
    CHECK(atomic_load_explicit(
              &call_tail_publication_hook_armed, memory_order_acquire) == 0);
    const int pending_before_unrelated =
        yz_spurs_test_pending_snapshot_count(jc);
    CHECK(pending_before_unrelated >= 0);
    /* The main list and CALL target make watch_lo/watch_hi cover this address,
     * but it was never a command slot.  A broad-envelope observer used to
     * queue this ordinary value as an immutable SYNC generation, exactly like
     * the unrelated 0x015C7Bxx values in acceptance run 20. */
    put64(guest + 0x63800, 0x02ull);
    cellSpursNotifyGuestWrite(0x63800, 8);
    CHECK(yz_spurs_test_pending_snapshot_count(jc) ==
          pending_before_unrelated);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_streamed_job_generation(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x36000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x37000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x37800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x38000);
    uint64_t* commands = (uint64_t*)(guest + 0x39000);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);

    /* Establish a consumed circular generation, then begin the next one with
     * only its first job published.  Publishing the second job while the
     * first is executing extends the current generation; it must not be
     * replayed as another generation when the worker reaches the empty tail. */
    put64(commands + 0, 0xe000);
    put64(commands + 1, 0xe000);
    put64(commands + 2, 0x0000000000039013ull); /* empty self NEXT */
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-stream"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority, 1, 0,
              0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 0;
    job_entered = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(wait_value(&job_runs, 2));
    brief_sleep();

    put64(commands + 1, 0x000000000003900bull); /* empty self NEXT */
    cellSpursNotifyGuestWrite(0x39008, 8);
    put64(commands + 0, 0x0000000000039003ull); /* empty self NEXT */
    cellSpursNotifyGuestWrite(0x39000, 8);
    job_hold = 1;
    job_entered = 0;
    put64(commands + 0, 0xe000);
    cellSpursNotifyGuestWrite(0x39000, 8);
    CHECK(wait_value(&job_entered, 1));
    put64(commands + 1, 0xe000);
    cellSpursNotifyGuestWrite(0x39008, 8);
    job_hold = 0;
    CHECK(wait_value(&job_runs, 4));
    brief_sleep();
    CHECK(job_runs == 4);
    CHECK(job_abi_ok);

    /* A grabbed snapshot may still contain a command whose guest slot has
     * already been recycled for the following generation.  Executing the
     * older immutable command must not cancel that newer refill candidate. */
    put64(commands + 0, 0x0000000000039003ull);
    cellSpursNotifyGuestWrite(0x39000, 8);
    put64(commands + 1, 0x000000000003900bull);
    cellSpursNotifyGuestWrite(0x39008, 8);
    put64(commands + 1, 0xe000); /* body published before the generation head */
    job_hold = 1;
    job_entered = 0;
    put64(commands + 0, 0xe000);
    cellSpursNotifyGuestWrite(0x39000, 8);
    CHECK(wait_value(&job_entered, 1));
    put64(commands + 1, 0x000000000003900bull);
    cellSpursNotifyGuestWrite(0x39008, 8);
    put64(commands + 1, 0xe000);
    cellSpursNotifyGuestWrite(0x39008, 8);
    job_hold = 0;
    CHECK(wait_value(&job_runs, 7));
    brief_sleep();
    CHECK(job_runs == 7);
    CHECK(cellSpursShutdownJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_repeated_barrier_publications(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x3a000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x3b000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x3b800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x3c000);
    uint64_t* commands = (uint64_t*)(guest + 0x3d000);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands + 0, 0xe000);
    put64(commands + 1, 0x000000000003d00bull); /* empty self NEXT */
    put64(commands + 2, 0);
    put64(commands + 3, 0x000000000003d009ull); /* RESET_PC to slot 1 */
    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-repeat-publication"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority, 1, 0,
              0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 1;
    job_entered = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(wait_value(&job_entered, 1));

    /* Two identical stores before the command is consumed are one observable
     * main-memory generation, not an event queue. Keep only the newest claim;
     * otherwise a per-frame rewrite grows an unbounded snapshot backlog and
     * eventually replays ancient descriptor storage. */
    put64(commands + 1, 0x0000000000000002ull);
    put64(commands + 2, 0xe000);
    cellSpursNotifyGuestWrite(0x3d008, 8);
    cellSpursNotifyGuestWrite(0x3d008, 8);
    put64(commands + 1, 0x7f); /* live tail after both claimed generations */
    job_hold = 0;
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 2);
    CHECK(job_abi_ok);
    CHECK(yz_spurs_test_pending_snapshot_count(jc) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_long_lived_job_chain(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x25000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x26000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x26800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x27000);
    uint64_t* commands = (uint64_t*)(guest + 0x30000);
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    /* Circular production chains have no documented command-lifetime cap.
     * Put END just beyond the old host safety limit so a successful join
     * proves that 65,536 ordinary commands do not terminate the chain. */
    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(commands, 0, (65536u + 1u) * sizeof(*commands));
    put64(commands + 65536, 0x7f);
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority, 1, 0,
              0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_far_command_wakeup(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x2a000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x2b000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x2b800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x2c000);
    uint64_t* commands = (uint64_t*)(guest + 0xc0000);
    uint64_t* far_commands = (uint64_t*)(guest + 0xd0000);
    uint8_t* descriptor = guest + 0xe000;
    uint8_t* binary = guest + 0xf000;
    uint8_t* inout = guest + 0x10000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(descriptor, 0, 0x40);
    memset(binary, 0x5a, 16);
    for (uint32_t i = 0; i < 16; ++i) inout[i] = (uint8_t)i;
    put64(descriptor, 0xf000);
    put16(descriptor + 8, 1);
    put16(descriptor + 0x0a, 8);
    put32(descriptor + 0x10, 1);
    put32(descriptor + 0x14, 16);
    put64(descriptor + 0x30, ((uint64_t)16 << 32) | 0x10000);
    put64(commands, 0x00000000000d0003ull); /* NEXT to a distant ring slot */
    put64(far_commands + 0, 0x00000000000d0003ull); /* empty self NEXT */
    put64(far_commands + 1, 0x7f);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_job, "synthetic-job-far-wakeup"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority, 1, 0,
              0, 0, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    job_runs = 0;
    job_abi_ok = 0;
    job_hold = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(wait_u32_at_least(&g_native_spurs_ppu_watch_hi, 0xd0008));
    /* A distant NEXT used to widen one global min/max envelope across this
     * entire 64 KiB gap.  The exact router must retain the real target while
     * proving that an ordinary write in the middle has no SPURS recipient. */
    CHECK(yz_spurs_test_guest_write_route_count(0xc8000, 8) == 0);
    CHECK(yz_spurs_test_guest_write_route_count(0xd0000, 8) == 1);
    /* Exercise the correctness path used after the fixed router saturates.
     * Put its exact alias at a page boundary, then verify every write shape
     * that previously risked being lost by a coarse single-page check. */
    CHECK(yz_spurs_test_guest_write_route_count(0x1f0000, 8) == 0);
    CHECK(yz_spurs_test_guest_write_route_overflow_alias(
              0xd0000, 0x1f0000) == 0);
    CHECK(yz_spurs_test_guest_write_route_count(0x1f0002, 2) == 1); /* partial */
    CHECK(yz_spurs_test_guest_write_route_count(0x1f0003, 4) == 1); /* unaligned */
    CHECK(yz_spurs_test_guest_write_route_count(0x1efff0, 0x40) == 1); /* bulk */
    CHECK(yz_spurs_test_guest_write_route_count(0x1effff, 2) == 1); /* cross-page */
    CHECK(yz_spurs_test_guest_write_route_count(0x1f0800, 8) == 0);
    put64(far_commands, 0xe000);
    cellSpursNotifyPpuGuestWrite(0xd0000, 8);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(job_runs == 1);
    CHECK(job_abi_ok);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_job_guest_range_validation(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x50000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x51000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x51800);
    CellSpursJobChain* list_jc = (CellSpursJobChain*)(guest + 0x52000);
    CellSpursJobChain* desc_jc = (CellSpursJobChain*)(guest + 0x52800);
    uint64_t* list_commands = (uint64_t*)(guest + 0x53000);
    uint64_t* desc_commands = (uint64_t*)(guest + 0x53100);
    uint8_t* list = guest + 0x54000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(list_jc, 0, sizeof(*list_jc));
    memset(desc_jc, 0, sizeof(*desc_jc));
    /* The title places its chain and command list in sys_memory storage.
     * This production-range classifier caught the regression where the
     * safety check accepted only low ELF memory and PPU stacks. */
    CHECK(cellSpursTestProductionMainStorageRange(0x4019ca80u, 8));
    CHECK(cellSpursTestProductionMainStorageRange(0x4ffffff8u, 8));
    CHECK(!cellSpursTestProductionMainStorageRange(0x4ffffffcu, 8));
    CHECK(!cellSpursTestProductionMainStorageRange(0x20000000u, 8));
    CHECK(!cellSpursTestProductionMainStorageRange(0xfffffffcu, 8));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);

    /* Creation must reject an aligned command EA just beyond guest memory,
     * rather than dereferencing the reserved host address range. */
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, (uint64_t*)(guest + 0x53000), 0x40,
              1, priority, 1, 0, 0, 1, 0, 0x100, 1) == 0);
    put32(ja->bytes + 0x08, MEM_SIZE);
    CHECK(cellSpursCreateJobChainWithAttribute(
              spurs, list_jc, ja) == CELL_SPURS_JOB_ERROR_FAULT);

    /* A syntactically valid JobList whose count would wrap/walk beyond guest
     * memory must fail before the first descriptor access. */
    put32(list + 0x00, 0xffffffffu);
    put32(list + 0x04, 0x40);
    put64(list + 0x08, 0x10000);
    put64(list_commands + 0, 0x0000000000054006ull);
    put64(list_commands + 1, 0x7f);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, list_commands, 0x40, 1, priority,
              1, 0, 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, list_jc, ja) == 0);
    CHECK(cellSpursRunJobChain(list_jc) == 0);
    CHECK(cellSpursJoinJobChain(list_jc) == CELL_SPURS_JOB_ERROR_FAULT);

    /* Direct JOB acquisition validates the full descriptor span too. */
    put64(desc_commands + 0, 0x00000000001ffff0ull);
    put64(desc_commands + 1, 0x7f);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, desc_commands, 0x40, 1, priority,
              1, 0, 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, desc_jc, ja) == 0);
    CHECK(cellSpursRunJobChain(desc_jc) == 0);
    CHECK(cellSpursJoinJobChain(desc_jc) == CELL_SPURS_JOB_ERROR_FAULT);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_joblist_late_publication(void)
{
    CellSpurs* spurs = (CellSpurs*)(guest + 0x56000);
    CellSpursAttribute* sa = (CellSpursAttribute*)(guest + 0x57000);
    CellSpursJobChainAttribute* ja =
        (CellSpursJobChainAttribute*)(guest + 0x57800);
    CellSpursJobChain* jc = (CellSpursJobChain*)(guest + 0x58000);
    uint64_t* commands = (uint64_t*)(guest + 0x59000);
    uint8_t* list = guest + 0x5a000;
    uint8_t* descriptor = guest + 0x5b000;
    uint8_t* binary = guest + 0x5c000;
    uint8_t priority[8] = {1,1,1,1,1,1,1,1};

    memset(spurs, 0, sizeof(*spurs));
    memset(jc, 0, sizeof(*jc));
    memset(list, 0, 16);
    memset(descriptor, 0, 0x40);
    memset(binary, 0x9d, 16);
    put64(descriptor + 0x00, 0x5c000);
    put16(descriptor + 0x08, 1);
    put32(list + 0x00, 1);       /* count visible first */
    put32(list + 0x04, 0);       /* size not published yet */
    put64(list + 0x08, 0);       /* descriptor EA not published yet */
    put64(commands + 0, 0x000000000005a006ull);
    put64(commands + 1, 0x7f);

    spu_workload_reset();
    CHECK(spu_workload_register_direct(
              spu_workload_fingerprint(binary, 16), 16,
              synthetic_joblist_job, "synthetic-joblist-job"));
    CHECK(cellSpursAttributeInitialize(sa, 2, 100, 1000, 0) == 0);
    CHECK(cellSpursInitializeWithAttribute(spurs, sa) == 0);
    CHECK(_cellSpursJobChainAttributeInitialize(
              3, 0x475001, ja, commands, 0x40, 1, priority,
              1, 0, 0, 1, 0, 0x100, 1) == 0);
    CHECK(cellSpursCreateJobChainWithAttribute(spurs, jc, ja) == 0);
    joblist_publication_hook_ea = 0x5a000;
    joblist_publication_descriptor_ea = 0x5b000;
    atomic_store_explicit(
        &joblist_publication_hook_armed, 1, memory_order_release);
    joblist_job_runs = 0;
    CHECK(cellSpursRunJobChain(jc) == 0);
    CHECK(cellSpursJoinJobChain(jc) == 0);
    CHECK(joblist_job_runs == 1);
    CHECK(atomic_load_explicit(
              &joblist_publication_hook_armed, memory_order_acquire) == 0);
    CHECK(cellSpursFinalize(spurs) == 0);
    return 0;
}

static int test_ticket_mutex(void)
{
    CellSyncMutex* m = (CellSyncMutex*)(guest + 0x11000);
    CHECK(cellSyncMutexInitialize(m) == 0);
    CHECK(cellSyncMutexTryLock(m) == 0);
    CHECK(guest[0x11000] == 0 && guest[0x11001] == 0 &&
          guest[0x11002] == 0 && guest[0x11003] == 1);
    CHECK(cellSyncMutexTryLock(m) == CELL_SYNC_ERROR_BUSY);
    CHECK(cellSyncMutexUnlock(m) == 0);
    CHECK(guest[0x11000] == 0 && guest[0x11001] == 1 &&
          guest[0x11002] == 0 && guest[0x11003] == 1);
    return 0;
}

static int test_unknown_job_inventory_capture(void)
{
    const char* path = "native_spurs_unknown_job_test.jsonl";
    char line[1024];
    remove(path);
    CHECK(cellSpursTestCaptureUnknownJob(
              path, 0x401b2900u, 0x40u, 0x01241400u, 0x11280u,
              0x3dafc265bd3e4da2ull, 0xe400u) == 1);
    /* Identical evidence is deduplicated in memory and on disk. */
    CHECK(cellSpursTestCaptureUnknownJob(
              path, 0x401b2900u, 0x40u, 0x01241400u, 0x11280u,
              0x3dafc265bd3e4da2ull, 0xe400u) == 0);
    FILE* file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fgets(line, sizeof(line), file) != NULL);
    CHECK(fgetc(file) == EOF);
    fclose(file);
    CHECK(strstr(line, "\"binary_ea\":\"0x01241400\"") != NULL);
    CHECK(strstr(line, "\"binary_size\":\"0x11280\"") != NULL);
    CHECK(strstr(line, "\"fingerprint\":\"0x3DAFC265BD3E4DA2\"") != NULL);
    CHECK(remove(path) == 0);
    return 0;
}

int main(void)
{
    if (test_task_bit_reservation_coherence()) return 1;
    if (test_layouts() || test_native_syscall_return_depth() ||
        test_multiple_instances() ||
        test_task_poll_workload_yield() ||
        test_task_event_queue() || test_spurs_queue_lock_order() ||
        test_job_logical_binary_size() ||
        test_job_sub_qword_dma_layout() ||
        test_large_job_slot_fallback() ||
        test_job_chain() || test_unnotified_pending_ticket_orbit() ||
        test_job_descriptor_snapshot() ||
        test_job_descriptor_stable_acquisition() ||
        test_job_barrier_snapshot() ||
        test_incomplete_barrier_descriptor_publication() ||
        test_call_tail_late_publication() ||
        test_streamed_job_generation() ||
        test_repeated_barrier_publications() ||
        test_long_lived_job_chain() || test_far_command_wakeup() ||
        test_job_guest_range_validation() || test_joblist_late_publication() ||
        test_ticket_mutex() || test_unknown_job_inventory_capture()) return 1;
    puts("native_spurs_tests: PASS");
    return 0;
}
