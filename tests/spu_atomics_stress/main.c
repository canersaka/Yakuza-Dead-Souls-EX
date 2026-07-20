/*
 * ps3recomp - tests/spu_atomics_stress/main.c
 *
 * Conformance/stress harness for the SPU MFC reservation/atomics model:
 * runtime/spu/spu_dma.h's GETLLAR/PUTLLC/PUTLLUC handling (resv_ea/resv_data/
 * resv_gen, the lock-free GETLLAR fast path) + runtime/spu/spu_channels.c's
 * spu_coh_notify_write/spu_coh_gen/spu_lockline_lock coherence bridge.
 *
 * Links the REAL runtime/spu/spu_channels.c (not a reimplementation), so SPU
 * MFC commands are driven the same way lifted guest code drives them: via
 * spu_wrch()/spu_rdch() channel calls, which route through spu_channels.c's
 * mfc_for() per-context registry -- the SAME registry spu_coh_notify_write()
 * walks to invalidate peer reservations. Driving mfc_submit()/mfc_channel_write()
 * directly with a private mfc_engine (as runtime/spu/tests/test_dma_main.c does
 * for its single-context unit test) would NOT be visible to that registry and
 * would produce false "reservation never invalidated" failures that are a
 * harness artifact, not a runtime bug -- see the report for this reasoning.
 *
 * "PPU-side" writers (plain store, reservation-store CAS) are NOT linked from
 * yakuza/shims.cpp (huge unrelated dependency surface); they call the exact
 * same underlying primitives (spu_coh_is_reserved/spu_lockline_lock/
 * spu_coh_notify_write) shims.cpp's VM_WRITE_COH macro and ppu_stwcx32 use, so
 * this exercises the real shared coherence code, just not through the game
 * runtime's PPU context machinery.
 *
 * Usage: spu_atomics_stress.exe [seed]   (default seed 0)
 * Exit code 0 = all invariants held. Nonzero = at least one FAILed.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <intrin.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "spu_dma.h"
#include "spu_context.h"

/* ---------------------------------------------------------------------------
 * Guest memory: reserve a full 4GB address space (like the real runtime's
 * vm_base -- yakuza/shims.cpp/main.cpp reserve+commit the same way) and commit
 * only the small windows this harness's test lines live in.
 * -----------------------------------------------------------------------*/
uint8_t* vm_base = NULL;

static void vm_init(void)
{
    vm_base = (uint8_t*)VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!vm_base) {
        fprintf(stderr, "FATAL: VirtualAlloc reserve 4GB failed, err=%lu\n", GetLastError());
        exit(97);
    }
}

static void vm_commit_page(uint32_t addr)
{
    uint32_t base = addr & ~0xFFFu;
    if (!VirtualAlloc(vm_base + base, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "FATAL: VirtualAlloc commit failed at 0x%08X, err=%lu\n", base, GetLastError());
        exit(97);
    }
}

/* spu_context_init (spu_context.h) reads the guest timebase; decrementer
 * semantics are irrelevant to atomics conformance, so a monotonic ms-based
 * stand-in is fine. */
uint64_t ppu_timebase_now(void) { return (uint64_t)GetTickCount64() * 79800ull; }

/* ---------------------------------------------------------------------------
 * Coherence primitives under test (runtime/spu/spu_channels.c). spu_dma.h
 * already declares spu_lockline_lock/unlock; the rest are declared locally
 * (as `extern`) inside spu_dma.h's own functions, so re-declare them here at
 * file scope for the PPU-side writer helpers below.
 * -----------------------------------------------------------------------*/
extern int      spu_coh_is_reserved(uint32_t addr);
extern uint32_t spu_coh_gen(uint32_t addr);
extern void     spu_coh_notify_write(uint32_t ea);

/* SPU channel ABI (defined in the real spu_channels.c; not declared in any
 * shared header outside the lifter's generated per-test spu_recomp.h). */
extern void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
extern u128 spu_rdch(spu_context* ctx, uint32_t channel);
extern uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

/* ---------------------------------------------------------------------------
 * Link-only stubs: spu_channels.c's SPU_WrOutIntrMbox handler (doorbell/
 * throw_event routing) and its YZ_TS_WATCH diagnostic reference lv2 syscall
 * plumbing (sys_event_queue_push_by_id, sys_event_flag_set_by_id,
 * spu_group_spup_queue) and two globals (g_yz_spurs_taskset/g_yz_codec_taskset)
 * that this harness never exercises -- it only drives MFC channels (LSA/EAH/
 * EAL/Size/TagID/Cmd/RdAtomicStat), never SPU_WrOutIntrMbox. These symbols are
 * still referenced unconditionally at compile time (not dead-code-eliminated
 * behind the getenv() gates, which are runtime checks), so the linker needs a
 * definition; they must never actually be called by anything this harness
 * does, so each one asserts if it is. -----------------------------------*/
uint32_t g_yz_spurs_taskset = 0;
uint32_t g_yz_codec_taskset = 0;
uint32_t g_yz_parked_pub_ea = 0;
uint32_t yz_guest_addr_from_host(const void* rip) { (void)rip; return 0; }
uint32_t yz_thread_current_id(void) { return 0; }
uint32_t spu_group_spup_queue(uint32_t group_id, uint32_t spup)
{
    (void)group_id; (void)spup;
    fprintf(stderr, "FATAL: spu_group_spup_queue stub called -- harness issued an unexpected "
            "SPU_WrOutIntrMbox doorbell\n");
    abort();
}
int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source, uint64_t d1, uint64_t d2, uint64_t d3)
{
    (void)queue_id; (void)source; (void)d1; (void)d2; (void)d3;
    fprintf(stderr, "FATAL: sys_event_queue_push_by_id stub called unexpectedly\n");
    abort();
}
int64_t sys_event_flag_set_by_id(uint32_t flag_id, uint64_t bitpat)
{
    (void)flag_id; (void)bitpat;
    fprintf(stderr, "FATAL: sys_event_flag_set_by_id stub called unexpectedly\n");
    abort();
}

/* ---------------------------------------------------------------------------
 * PPU-side writer helpers -- mirror yakuza/shims.cpp's VM_WRITE_COH macro and
 * its stwcx-family reservation-store path EXACTLY (same primitives, no game-
 * runtime dependency): shims.cpp lines ~86-91 (VM_WRITE_COH) and ~294-311
 * (ppu_stwcx32-equivalent). Not copied from GPL code -- shims.cpp is our own
 * MIT-licensed file; this reproduces its logic against the same extern API.
 * -----------------------------------------------------------------------*/
static void ppu_write_coh(uint32_t addr, const void* src, unsigned n)
{
    if (spu_coh_is_reserved(addr)) {
        spu_lockline_lock();
        memcpy(vm_base + addr, src, n);
        spu_coh_notify_write(addr);
        spu_lockline_unlock();
    } else {
        memcpy(vm_base + addr, src, n);
    }
}

/* Returns 1 on a successful CAS commit, 0 on mismatch (stwcx. "failed"). */
static int ppu_stwcx32(uint32_t addr, uint32_t expected, uint32_t desired)
{
    int ok = 0;
    volatile long* p = (volatile long*)(vm_base + addr);
    if (spu_coh_is_reserved(addr)) {
        spu_lockline_lock();
        if (*(volatile uint32_t*)p == expected) {
            *(volatile uint32_t*)p = desired;
            spu_coh_notify_write(addr);
            ok = 1;
        }
        spu_lockline_unlock();
    } else {
        ok = (_InterlockedCompareExchange(p, (long)desired, (long)expected) == (long)expected);
    }
    return ok;
}

/* Raw bulk-writer stand-in for the ALREADY-DOCUMENTED gap noted in
 * spu_dma.h's own comments (~line 1000-1009): "host-side bulk writers (HLE
 * _sys_memset/_sys_memcpy, file reads) write guest memory WITHOUT bumping the
 * generation". No lock, no spu_coh_is_reserved check, no notify -- exactly
 * what those HLE paths do. Used only by the clearly-labeled bonus test 1b. */
static void raw_bulk_write(uint32_t addr, const void* src, unsigned n)
{
    memcpy(vm_base + addr, src, n);
}

/* ---------------------------------------------------------------------------
 * SPU-side MFC helpers: drive channels the way lifted guest code does.
 * -----------------------------------------------------------------------*/
static void mfc_stage(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    spu_wrch(ctx, MFC_LSA, spu_make_preferred_u32(lsa));
    spu_wrch(ctx, MFC_EAH, spu_make_preferred_u32(0));
    spu_wrch(ctx, MFC_EAL, spu_make_preferred_u32(ea));
    spu_wrch(ctx, MFC_Size, spu_make_preferred_u32(size));
    spu_wrch(ctx, MFC_TagID, spu_make_preferred_u32(tag));
}
static uint32_t mfc_go(spu_context* ctx, uint32_t cmd)
{
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(cmd));
    u128 s = spu_rdch(ctx, MFC_RdAtomicStat);
    return s._u32[0];
}

/* Global reusable SPU-context pool. mfc_for()'s registry (spu_channels.c) is
 * capped at SPU_MAX_CONTEXTS=8 and keys on ctx POINTER IDENTITY, never
 * releasing a slot -- so every test in this binary must reuse the SAME <=8
 * persistent contexts (re-initialized per test) rather than allocating fresh
 * ones, or extra contexts beyond 8 silently fall back to ONE SHARED engine
 * (a harness artifact, not a runtime bug). No single test below uses more
 * than 6 of the 8 slots. */
#define SPU_POOL_N 8
static spu_context g_spu_pool[SPU_POOL_N];

static uint32_t g_seed;
static uint32_t xorshift32(uint32_t* s)
{
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}

static LONG volatile g_fail_count = 0;
static void fail(const char* test, const char* fmt, ...)
{
    InterlockedIncrement(&g_fail_count);
    fprintf(stderr, "[FAIL] %s: ", test);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n"); fflush(stderr);
}
static ULONGLONG now_ms(void) { return GetTickCount64(); }

/* =========================================================================
 * TEST 0 -- ATOMIC STATUS CHANNEL LIFECYCLE
 *
 * Each completed lock-line command publishes one readable status entry.
 * Reading that entry consumes it; channel-count probes never consume it.
 * ========================================================================= */
#define EA_CHSTAT 0x40105000u

static int test0_atomic_status_channel(void)
{
    const char* name = "test0_atomic_status_channel";
    LONG before = g_fail_count;
    spu_context* ctx = &g_spu_pool[SPU_POOL_N - 1];
    uint32_t count, status;

    printf("[%s] ready/publish/consume sequence...\n", name);
    vm_commit_page(EA_CHSTAT);
    memset(vm_base + EA_CHSTAT, 0, 128);
    spu_context_init(ctx, 0xF000u);

    count = spu_rchcnt(ctx, MFC_RdAtomicStat);
    if (count != 0)
        fail(name, "fresh channel count=%u, expected 0", count);

    mfc_stage(ctx, 0x1000, EA_CHSTAT, 128, 0);
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(MFC_GETLLAR_CMD));
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 1 ||
        spu_rchcnt(ctx, MFC_RdAtomicStat) != 1)
        fail(name, "GETLLAR completion was not stably readable before consume");
    status = spu_rdch(ctx, MFC_RdAtomicStat)._u32[0];
    if (status != MFC_GETLLAR_SUCCESS)
        fail(name, "GETLLAR status=%u, expected %u", status, MFC_GETLLAR_SUCCESS);
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 0)
        fail(name, "GETLLAR status remained readable after consume");

    memset(ctx->ls + 0x1000, 0x5A, 128);
    mfc_stage(ctx, 0x1000, EA_CHSTAT, 128, 0);
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(MFC_PUTLLC_CMD));
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 1)
        fail(name, "PUTLLC success did not publish a status entry");
    status = spu_rdch(ctx, MFC_RdAtomicStat)._u32[0];
    if (status != MFC_PUTLLC_SUCCESS)
        fail(name, "PUTLLC status=%u, expected success", status);
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 0)
        fail(name, "PUTLLC success remained readable after consume");

    mfc_stage(ctx, 0x1000, EA_CHSTAT, 128, 0);
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(MFC_PUTLLC_CMD));
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 1)
        fail(name, "PUTLLC failure did not publish a status entry");
    status = spu_rdch(ctx, MFC_RdAtomicStat)._u32[0];
    if (status != MFC_PUTLLC_FAILURE)
        fail(name, "second PUTLLC status=%u, expected reservation failure", status);
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 0)
        fail(name, "PUTLLC failure remained readable after consume");

    mfc_stage(ctx, 0x1000, EA_CHSTAT, 128, 0);
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(MFC_PUTLLUC_CMD));
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 1)
        fail(name, "PUTLLUC completion did not publish a status entry");
    status = spu_rdch(ctx, MFC_RdAtomicStat)._u32[0];
    if (status != MFC_PUTLLUC_SUCCESS)
        fail(name, "PUTLLUC status=%u, expected %u", status, MFC_PUTLLUC_SUCCESS);
    if (spu_rchcnt(ctx, MFC_RdAtomicStat) != 0)
        fail(name, "PUTLLUC status remained readable after consume");

    printf("[%s] -> %s\n", name, g_fail_count == before ? "PASS" : "FAIL");
    return g_fail_count == before ? 0 : 1;
}

/* =========================================================================
 * TEST 1 -- RESERVATION-KILL (the ABA case)
 *
 * Any committed write to a reserved line between a GETLLAR and a later PUTLLC
 * must fail that PUTLLC -- even if the line's bytes happen to read back
 * identical to the GETLLAR snapshot (the ABA case: two writers alternate
 * between two fixed patterns, so the line is frequently byte-identical to
 * what it was, despite writes having landed).
 *
 * Oracle: a global monotonic "write epoch" (bumped by EVERY writer, of every
 * type, right after its commit) sampled by each reserver immediately after
 * its GETLLAR and again immediately before its PUTLLC attempt. If the epoch
 * differs, at least one commit landed in that window and a real reservation
 * MUST have been killed; a PUTLLC that reports SUCCESS anyway is a
 * violation, independent of what the actual bytes were (subsumes, and is
 * strictly stronger than, the byte-identical ABA case the spec calls out).
 * This can only under-count intervening writes (a commit landing in the
 * instant between the "before" sample and the real mfc_submit call is
 * invisible to the oracle but a correct runtime still fails it), so it can
 * only produce a real positive, never a false one.
 * ========================================================================= */
#define EA_ABA          0x40100000u
#define T1_DURATION_MS  4000
#define T1_MAX_ITERS    300000

static LONG volatile g_t1_epoch = 0;
static LONG volatile g_t1_stop = 0;

typedef struct { int slot; LONG volatile iters; LONG volatile violations; LONG volatile succ; } t1_reserver_arg;

static unsigned __stdcall t1_reserver(void* p)
{
    t1_reserver_arg* a = (t1_reserver_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x1000u + (uint32_t)a->slot);
    uint32_t rng = g_seed ^ (0xB5297A4Du * (uint32_t)(a->slot + 7));

    while (!g_t1_stop && a->iters < T1_MAX_ITERS) {
        mfc_stage(ctx, 0x1000, EA_ABA, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        /* Race-free oracle: spu_coh_gen() is bumped SYNCHRONOUSLY, under the
         * SAME spu_lockline_lock() critical section as the actual memory
         * write, by every one of the four writer classes' spu_coh_notify_write
         * call (spu_dma.h ~1611/1623/spu_channels.c). A separately-bumped
         * counter incremented by the WRITER thread AFTER its mfc_go() call
         * returns (this file's earlier draft) is NOT race-free: the increment
         * can be arbitrarily delayed by OS scheduling relative to the actual
         * commit, so a reserver can observe a stale "unchanged" epoch sample
         * across a round where nothing really intervened, OR a delayed bump
         * from an OLD (already-snapshotted) commit can land between this
         * round's two samples and manufacture a false violation. Sampling the
         * runtime's own generation counter has no such gap. */
        uint32_t gen_at_getllar = spu_coh_gen(EA_ABA & ~127u);
        long epoch_at_getllar = g_t1_epoch; /* informational only (commit-count context in FAIL messages) */

        /* Widen the race window randomly so different interleavings get hit
         * across the run (some iterations back-to-back, some delayed). */
        uint32_t r = xorshift32(&rng) & 0xFF;
        if (r < 60) { /* ~23%: yield the core briefly */
            SwitchToThread();
        } else if (r < 90) { /* ~12%: a handful of pause spins */
            for (int i = 0; i < 40; i++) _mm_pause();
        }

        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, 0xC3, 128);
        buf[0] = (uint8_t)a->slot;
        uint32_t gen_before_putllc = spu_coh_gen(EA_ABA & ~127u);
        long epoch_before_putllc = g_t1_epoch;

        mfc_stage(ctx, 0x1000, EA_ABA, 128, 0);
        uint32_t stat = mfc_go(ctx, MFC_PUTLLC_CMD);

        if (stat == MFC_PUTLLC_SUCCESS) {
            if (gen_before_putllc != gen_at_getllar) {
                InterlockedIncrement(&a->violations);
                fail("test1_reservation_kill",
                     "reserver slot=%d: PUTLLC SUCCEEDED on EA=0x%08X despite the coherence "
                     "generation advancing from %u to %u between this GETLLAR and this PUTLLC "
                     "(informational commit-epoch %ld->%ld) -- ABA/reservation-kill violation",
                     a->slot, EA_ABA, gen_at_getllar, gen_before_putllc,
                     epoch_at_getllar, epoch_before_putllc);
            }
            InterlockedIncrement(&g_t1_epoch);
            InterlockedIncrement(&a->succ);
        }
        InterlockedIncrement(&a->iters);
    }
    return 0;
}

/* Writer A: peer SPU PUTLLC (GETLLAR then PUTLLC a fresh alternating pattern). */
typedef struct { int slot; LONG volatile commits; } t1_writer_arg;

static unsigned __stdcall t1_writer_putllc(void* p)
{
    t1_writer_arg* a = (t1_writer_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x2000u + (uint32_t)a->slot);
    int phase = 0;
    while (!g_t1_stop) {
        mfc_stage(ctx, 0x1000, EA_ABA, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, phase ? 0x5A : 0xA5, 128);
        phase ^= 1;
        mfc_stage(ctx, 0x1000, EA_ABA, 128, 0);
        if (mfc_go(ctx, MFC_PUTLLC_CMD) == MFC_PUTLLC_SUCCESS) {
            InterlockedIncrement(&g_t1_epoch);
            InterlockedIncrement(&a->commits);
        }
    }
    return 0;
}

/* Writer B: unconditional PUTLLUC. */
static unsigned __stdcall t1_writer_putlluc(void* p)
{
    t1_writer_arg* a = (t1_writer_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x3000u + (uint32_t)a->slot);
    int phase = 0;
    while (!g_t1_stop) {
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, phase ? 0x5A : 0xA5, 128);
        phase ^= 1;
        mfc_stage(ctx, 0x1000, EA_ABA, 128, 0);
        mfc_go(ctx, MFC_PUTLLUC_CMD);
        InterlockedIncrement(&g_t1_epoch);
        InterlockedIncrement(&a->commits);
    }
    return 0;
}

/* Writer C: plain PUT (mfc_do_transfer's unlocked-copy path). */
static unsigned __stdcall t1_writer_plainput(void* p)
{
    t1_writer_arg* a = (t1_writer_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x4000u + (uint32_t)a->slot);
    int phase = 0;
    while (!g_t1_stop) {
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, phase ? 0x5A : 0xA5, 128);
        phase ^= 1;
        mfc_stage(ctx, 0x1000, EA_ABA, 128, 0);
        mfc_go(ctx, MFC_PUT_CMD);
        InterlockedIncrement(&g_t1_epoch);
        InterlockedIncrement(&a->commits);
    }
    return 0;
}

/* Writer D: PPU reservation-store (stwcx-style) CAS on a 4-byte word inside
 * the same 128B granule -- exercises "PPU reservation store path" from the
 * spec while still killing the WHOLE line's reservations (spu_coh_notify_write
 * line-aligns internally, matching CBEA's cache-line-granule invalidation). */
static unsigned __stdcall t1_writer_ppu_cas(void* p)
{
    t1_writer_arg* a = (t1_writer_arg*)p;
    uint32_t cur;
    memcpy(&cur, vm_base + EA_ABA + 64, 4);
    while (!g_t1_stop) {
        uint32_t desired = (cur == 0x11111111u) ? 0x22222222u : 0x11111111u;
        if (ppu_stwcx32(EA_ABA + 64, cur, desired)) {
            cur = desired;
            InterlockedIncrement(&g_t1_epoch);
            InterlockedIncrement(&a->commits);
        } else {
            memcpy(&cur, vm_base + EA_ABA + 64, 4); /* re-read, like a real lwarx retry */
        }
    }
    return 0;
}

static int test1_reservation_kill(void)
{
    const char* name = "test1_reservation_kill";
    LONG before = g_fail_count;
    printf("[%s] EA=0x%08X, 2 reservers x 4 writer classes (peer PUTLLC, PUTLLUC, "
           "plain PUT, PPU reservation-store CAS), %dms...\n", name, EA_ABA, T1_DURATION_MS);

    vm_commit_page(EA_ABA);
    memset(vm_base + EA_ABA, 0, 128);

    g_t1_stop = 0;
    t1_reserver_arg r0 = { 0, 0, 0 }, r1 = { 1, 0, 0 };
    t1_writer_arg wA = { 2, 0 }, wB = { 3, 0 }, wC = { 4, 0 }, wD = { 5, 0 };

    HANDLE hR0 = (HANDLE)_beginthreadex(NULL, 0, t1_reserver, &r0, 0, NULL);
    HANDLE hR1 = (HANDLE)_beginthreadex(NULL, 0, t1_reserver, &r1, 0, NULL);
    HANDLE hA = (HANDLE)_beginthreadex(NULL, 0, t1_writer_putllc, &wA, 0, NULL);
    HANDLE hB = (HANDLE)_beginthreadex(NULL, 0, t1_writer_putlluc, &wB, 0, NULL);
    HANDLE hC = (HANDLE)_beginthreadex(NULL, 0, t1_writer_plainput, &wC, 0, NULL);
    HANDLE hD = (HANDLE)_beginthreadex(NULL, 0, t1_writer_ppu_cas, &wD, 0, NULL);

    Sleep(T1_DURATION_MS);
    InterlockedExchange(&g_t1_stop, 1);

    WaitForSingleObject(hR0, 5000); WaitForSingleObject(hR1, 5000);
    WaitForSingleObject(hA, 2000); WaitForSingleObject(hB, 2000);
    WaitForSingleObject(hC, 2000); WaitForSingleObject(hD, 2000);
    CloseHandle(hR0); CloseHandle(hR1); CloseHandle(hA); CloseHandle(hB); CloseHandle(hC); CloseHandle(hD);

    int ok = (g_fail_count == before);
    printf("[%s] reserver iters=%ld/%ld violations=%ld/%ld | writer commits: putllc=%ld putlluc=%ld "
           "plainput=%ld ppu_cas=%ld -> %s\n",
           name, r0.iters, r1.iters, r0.violations, r1.violations,
           wA.commits, wB.commits, wC.commits, wD.commits, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 1b (bonus, NOT one of the four spec'd writer classes) -- reproduces
 * the gap spu_dma.h's own comments (~line 1000-1009) already document: a
 * "host-side bulk writer" (HLE _sys_memset/_sys_memcpy/file-read paths) writes
 * guest memory via a raw memcpy with NO spu_coh_is_reserved check and NO
 * spu_coh_notify_write call. Same oracle as test 1, but the only writer is
 * raw_bulk_write(). Reported separately so a FAIL here is not conflated with
 * the four required invariant-1 writer classes (which are expected to pass).
 * ========================================================================= */
#define EA_ABA_BULK 0x40101000u

static LONG volatile g_t1b_epoch = 0;
static LONG volatile g_t1b_stop = 0;

static unsigned __stdcall t1b_reserver(void* p)
{
    t1_reserver_arg* a = (t1_reserver_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x5000u + (uint32_t)a->slot);
    while (!g_t1b_stop && a->iters < T1_MAX_ITERS) {
        mfc_stage(ctx, 0x1000, EA_ABA_BULK, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        long epoch_at_getllar = g_t1b_epoch;
        SwitchToThread();
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, 0xC3, 128);
        long epoch_before_putllc = g_t1b_epoch;
        mfc_stage(ctx, 0x1000, EA_ABA_BULK, 128, 0);
        uint32_t stat = mfc_go(ctx, MFC_PUTLLC_CMD);
        if (stat == MFC_PUTLLC_SUCCESS) {
            if (epoch_before_putllc != epoch_at_getllar) {
                InterlockedIncrement(&a->violations);
                fail("test1b_bulk_writer_gap",
                     "reserver slot=%d: PUTLLC SUCCEEDED on EA=0x%08X despite %ld intervening "
                     "raw-bulk-writer commit(s) between GETLLAR (epoch=%ld) and PUTLLC "
                     "(epoch=%ld) -- reproduces the documented spu_dma.h ~line 1000-1009 gap",
                     a->slot, EA_ABA_BULK, epoch_before_putllc - epoch_at_getllar,
                     epoch_at_getllar, epoch_before_putllc);
            }
            InterlockedIncrement(&g_t1b_epoch);
            InterlockedIncrement(&a->succ);
        }
        InterlockedIncrement(&a->iters);
    }
    return 0;
}

static unsigned __stdcall t1b_writer_bulk(void* p)
{
    LONG volatile* commits = (LONG volatile*)p;
    int phase = 0;
    uint8_t buf[128];
    while (!g_t1b_stop) {
        memset(buf, phase ? 0x5A : 0xA5, 128);
        phase ^= 1;
        raw_bulk_write(EA_ABA_BULK, buf, 128);
        InterlockedIncrement(&g_t1b_epoch);
        InterlockedIncrement(commits);
    }
    return 0;
}

static int test1b_bulk_writer_gap(void)
{
    const char* name = "test1b_bulk_writer_gap (bonus, documented gap repro)";
    LONG before = g_fail_count;
    printf("[%s] EA=0x%08X, raw_bulk_write vs GETLLAR/PUTLLC, %dms...\n", name, EA_ABA_BULK, T1_DURATION_MS);

    vm_commit_page(EA_ABA_BULK);
    memset(vm_base + EA_ABA_BULK, 0, 128);

    g_t1b_stop = 0;
    t1_reserver_arg r0 = { 6, 0, 0 };
    LONG volatile commits = 0;

    HANDLE hR0 = (HANDLE)_beginthreadex(NULL, 0, t1b_reserver, &r0, 0, NULL);
    HANDLE hW = (HANDLE)_beginthreadex(NULL, 0, t1b_writer_bulk, (void*)&commits, 0, NULL);

    Sleep(T1_DURATION_MS);
    InterlockedExchange(&g_t1b_stop, 1);
    WaitForSingleObject(hR0, 5000); WaitForSingleObject(hW, 2000);
    CloseHandle(hR0); CloseHandle(hW);

    /* Decisive, race-free corroboration: the runtime's coherence generation
     * for this line can ONLY be bumped by spu_coh_notify_write, which
     * raw_bulk_write() never calls -- the SOLE other writer touching this
     * line is this test's own reserver, whose successful PUTLLCs go through
     * the normal spu_dma.h path and DO call notify. So post-join (no more
     * concurrent access), final_coh_gen must equal the reserver's own success
     * count (r0.succ) exactly if-and-only-if none of the bulk writer's
     * ~191M+ commits ever bumped it. This is checked with an exact
     * post-join read (no timing window at all, unlike the live dual-sample
     * violation counter above, whose exact per-round intervening-write COUNT
     * can be inflated by the cross-thread epoch-sampling slack noted in
     * test1's comment -- that counter's qualitative "REPRODUCED at all" does
     * not depend on the exact count, but this check does not rely on it
     * either). */
    uint32_t final_gen = spu_coh_gen(EA_ABA_BULK & ~127u);
    int gen_isolated_from_bulk_writer = ((long)final_gen == r0.succ);

    int violated = (r0.violations != 0) || !gen_isolated_from_bulk_writer;
    /* This test's PASS/FAIL is reported but NOT folded into g_fail_count's
     * pass/fail for the overall exit code in the caller's summary line --
     * see main(): it is printed as its own labeled verdict since it targets
     * an already-known/documented gap, not one of the four required classes. */
    printf("[%s] reserver iters=%ld violations=%ld reserver_own_successes=%ld "
           "bulk_writer_commits=%ld final_coh_gen=%u (gen==reserver_successes: %s, i.e. the bulk "
           "writer's commits contributed ZERO generation bumps) -> %s (pre-existing documented "
           "gap; NOT one of the four spec'd writer classes)\n",
           name, r0.iters, r0.violations, r0.succ, commits, final_gen,
           gen_isolated_from_bulk_writer ? "yes" : "no",
           violated ? "REPRODUCED" : "NOT REPRODUCED");
    (void)before;
    return violated ? 1 : 0;
}

/* =========================================================================
 * TEST 2 -- SNAPSHOT ATOMICITY (torn reads)
 *
 * A GETLLAR snapshot must never mix bytes from two different writers. Every
 * writer stamps the FULL 128-byte line with its own single repeated byte
 * value (id 1..K); a torn snapshot shows up as more than one distinct byte
 * value in a single 128-byte read.
 *
 * Includes both PUTLLUC writers (whose 128-byte copy runs INSIDE
 * spu_lockline_lock, spu_dma.h ~line 1622) and plain-PUT writers (whose copy,
 * spu_dma.h line 331 `memcpy(ea_ptr, ls_ptr, size)`, runs OUTSIDE the lock --
 * only the subsequent coherence-notify is serialized). That asymmetry is the
 * concrete mechanism this test targets: a plain PUT's unlocked memcpy racing
 * a peer's locked GETLLAR/PUTLLC memcpy on the SAME 128 bytes is an
 * unsynchronized concurrent access (UB in C; no atomicity guarantee from
 * ordinary memcpy at any granularity above the host's natural word size).
 * ========================================================================= */
#define EA_SNAP        0x40102000u
#define T2_DURATION_MS 4000
#define T2_WRITERS     4   /* 2 PUTLLUC + 2 plain-PUT */
#define T2_READERS     3

static LONG volatile g_t2_stop = 0;
static LONG volatile g_t2_torn = 0;
static LONG volatile g_t2_snapshots = 0;

typedef struct { int slot; int use_plain_put; uint8_t id; } t2_writer_arg;

static unsigned __stdcall t2_writer(void* p)
{
    t2_writer_arg* a = (t2_writer_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x6000u + (uint32_t)a->slot);
    while (!g_t2_stop) {
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, a->id, 128);
        mfc_stage(ctx, 0x1000, EA_SNAP, 128, 0);
        mfc_go(ctx, a->use_plain_put ? MFC_PUT_CMD : MFC_PUTLLUC_CMD);
    }
    return 0;
}

typedef struct { int slot; } t2_reader_arg;

static unsigned __stdcall t2_reader(void* p)
{
    t2_reader_arg* a = (t2_reader_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x7000u + (uint32_t)a->slot);
    while (!g_t2_stop) {
        mfc_stage(ctx, 0x1000, EA_SNAP, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        uint8_t* snap = ctx->ls + 0x1000;
        uint8_t first = snap[0];
        int mismatch_at = -1;
        for (int i = 1; i < 128; i++) {
            if (snap[i] != first) { mismatch_at = i; break; }
        }
        InterlockedIncrement(&g_t2_snapshots);
        if (mismatch_at >= 0) {
            InterlockedIncrement(&g_t2_torn);
            if (g_t2_torn <= 5) {
                char trace[512]; int o = 0;
                for (int i = 0; i < 128 && o < 480; i += 16) {
                    o += snprintf(trace + o, sizeof(trace) - o, " +%02X:%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                                  i, snap[i],snap[i+1],snap[i+2],snap[i+3],snap[i+4],snap[i+5],snap[i+6],snap[i+7],
                                  snap[i+8],snap[i+9],snap[i+10],snap[i+11],snap[i+12],snap[i+13],snap[i+14],snap[i+15]);
                }
                fail("test2_snapshot_atomicity",
                     "reader slot=%d: TORN snapshot at EA=0x%08X, first mismatch at byte offset %d "
                     "(expected uniform 0x%02X). Full 128-byte layout:%s",
                     a->slot, EA_SNAP, mismatch_at, first, trace);
            }
        }
    }
    return 0;
}

static int test2_snapshot_atomicity(void)
{
    const char* name = "test2_snapshot_atomicity";
    LONG before = g_fail_count;
    printf("[%s] EA=0x%08X, %d writers (2 PUTLLUC + 2 plain-PUT) x %d readers, %dms...\n",
           name, EA_SNAP, T2_WRITERS, T2_READERS, T2_DURATION_MS);

    vm_commit_page(EA_SNAP);
    memset(vm_base + EA_SNAP, 0, 128);

    g_t2_stop = 0; g_t2_torn = 0; g_t2_snapshots = 0;
    t2_writer_arg w[T2_WRITERS] = {
        { 0, 0, 0x11 }, { 1, 0, 0x22 }, /* PUTLLUC */
        { 2, 1, 0x33 }, { 3, 1, 0x44 }, /* plain PUT (unlocked copy path) */
    };
    t2_reader_arg r[T2_READERS] = { {4}, {5}, {6} };
    HANDLE hw[T2_WRITERS], hr[T2_READERS];
    for (int i = 0; i < T2_WRITERS; i++) hw[i] = (HANDLE)_beginthreadex(NULL, 0, t2_writer, &w[i], 0, NULL);
    for (int i = 0; i < T2_READERS; i++) hr[i] = (HANDLE)_beginthreadex(NULL, 0, t2_reader, &r[i], 0, NULL);

    Sleep(T2_DURATION_MS);
    InterlockedExchange(&g_t2_stop, 1);
    for (int i = 0; i < T2_WRITERS; i++) { WaitForSingleObject(hw[i], 2000); CloseHandle(hw[i]); }
    for (int i = 0; i < T2_READERS; i++) { WaitForSingleObject(hr[i], 2000); CloseHandle(hr[i]); }

    int ok = (g_fail_count == before);
    printf("[%s] snapshots examined=%ld torn=%ld -> %s\n", name, g_t2_snapshots, g_t2_torn, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 3 -- FAST-PATH COHERENCE (the lock-free GETLLAR fast path)
 *
 * spu_dma.h's lock-free fast path is HARDCODED to line 0x40197C80 (the SPURS
 * management line, s21 perf change) -- this test line's address is fixed to
 * match. One writer PUTLLUCs a monotonically increasing sequence number into
 * the line; each reader's GETLLAR must never return an OLDER sequence number
 * than one it already observed (a rollback would mean the fast path served a
 * cached copy the generation check should have rejected as stale).
 * ========================================================================= */
#define EA_FAST        0x40197C80u
#define T3_DURATION_MS 4000
#define T3_READERS     4

static LONG volatile g_t3_stop = 0;
static LONG volatile g_t3_seq = 0;

static unsigned __stdcall t3_writer(void* p)
{
    (void)p;
    spu_context* ctx = &g_spu_pool[0];
    spu_context_init(ctx, 0x8000u);
    uint32_t seq = 0;
    while (!g_t3_stop) {
        seq++;
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, 0, 128);
        memcpy(buf, &seq, 4);
        mfc_stage(ctx, 0x1000, EA_FAST, 128, 0);
        mfc_go(ctx, MFC_PUTLLUC_CMD);
        InterlockedExchange(&g_t3_seq, (LONG)seq);
        Sleep(0); /* let readers catch up between writes so more of their
                     GETLLARs hit the "same line, unchanged generation" fast
                     path (spu_dma.h ~line 1010) rather than racing every time */
    }
    return 0;
}

typedef struct { int slot; LONG last_seen; LONG polls; } t3_reader_arg;

static unsigned __stdcall t3_reader(void* p)
{
    t3_reader_arg* a = (t3_reader_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0x9000u + (uint32_t)a->slot);
    while (!g_t3_stop) {
        mfc_stage(ctx, 0x1000, EA_FAST, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        uint32_t seq; memcpy(&seq, ctx->ls + 0x1000, 4);
        a->polls++;
        if ((LONG)seq < a->last_seen) {
            fail("test3_fastpath_coherence",
                 "reader slot=%d: GETLLAR on EA=0x%08X returned seq=%u, OLDER than "
                 "already-observed seq=%ld -- fast-path served a stale cached copy",
                 a->slot, EA_FAST, seq, a->last_seen);
        } else {
            a->last_seen = (LONG)seq;
        }
    }
    return 0;
}

static int test3_fastpath_coherence(void)
{
    const char* name = "test3_fastpath_coherence";
    LONG before = g_fail_count;
    printf("[%s] EA=0x%08X (hardcoded fast-path line), 1 writer x %d readers, %dms...\n",
           name, EA_FAST, T3_READERS, T3_DURATION_MS);

    vm_commit_page(EA_FAST);
    memset(vm_base + EA_FAST, 0, 128);

    g_t3_stop = 0; g_t3_seq = 0;
    t3_reader_arg r[T3_READERS] = { {1,0,0}, {2,0,0}, {3,0,0}, {4,0,0} };
    HANDLE hw = (HANDLE)_beginthreadex(NULL, 0, t3_writer, NULL, 0, NULL);
    HANDLE hr[T3_READERS];
    for (int i = 0; i < T3_READERS; i++) hr[i] = (HANDLE)_beginthreadex(NULL, 0, t3_reader, &r[i], 0, NULL);

    Sleep(T3_DURATION_MS);
    InterlockedExchange(&g_t3_stop, 1);
    WaitForSingleObject(hw, 2000); CloseHandle(hw);
    for (int i = 0; i < T3_READERS; i++) { WaitForSingleObject(hr[i], 2000); CloseHandle(hr[i]); }

    int ok = (g_fail_count == before);
    int liveness_ok = 1;
    for (int i = 0; i < T3_READERS; i++) {
        printf("  reader slot=%d: polls=%ld last_seen=%ld\n", r[i].slot, r[i].polls, r[i].last_seen);
        if (r[i].last_seen == 0) liveness_ok = 0; /* never observed writer progress: vacuous test */
    }
    if (!liveness_ok) {
        fail(name, "at least one reader never observed the writer's sequence advance "
             "(vacuous run -- readers were not actually racing the writer)");
        ok = 0;
    }
    printf("[%s] final writer seq=%ld -> %s\n", name, g_t3_seq, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 4 -- LOST-WAKEUP EDGE
 *
 * A write that lands between a FAILED PUTLLC and the reserver's NEXT GETLLAR
 * must be observable in that GETLLAR. Deterministic 2-thread handshake (not a
 * free-for-all stress) so the ordering the spec describes is exact every
 * round: writer commits strictly happens-before the reserver's PUTLLC
 * attempt (guaranteed to fail, since the writer's commit invalidates the
 * reserver's reservation), and the reserver's immediately-following fresh
 * GETLLAR must see that exact committed sequence number.
 * ========================================================================= */
#define EA_EDGE        0x40103000u
#define T4_ROUNDS      5000

static LONG volatile g_t4_go = 0;     /* reserver -> writer: proceed */
static LONG volatile g_t4_done = 0;   /* writer -> reserver: committed */
static LONG volatile g_t4_seq = 0;    /* the seq the writer just committed */
static LONG volatile g_t4_stop_writer = 0;

static unsigned __stdcall t4_writer(void* p)
{
    (void)p;
    spu_context* ctx = &g_spu_pool[0];
    spu_context_init(ctx, 0xA000u);
    uint32_t seq = 0;
    while (!g_t4_stop_writer) {
        while (!g_t4_go && !g_t4_stop_writer) SwitchToThread();
        if (g_t4_stop_writer) break;
        seq++;
        uint8_t* buf = ctx->ls + 0x1000;
        memset(buf, 0, 128);
        memcpy(buf, &seq, 4);
        mfc_stage(ctx, 0x1000, EA_EDGE, 128, 0);
        mfc_go(ctx, MFC_PUTLLUC_CMD);
        InterlockedExchange(&g_t4_seq, (LONG)seq);
        InterlockedExchange(&g_t4_go, 0);
        InterlockedExchange(&g_t4_done, 1);
    }
    return 0;
}

static int test4_lost_wakeup_edge(void)
{
    const char* name = "test4_lost_wakeup_edge";
    LONG before = g_fail_count;
    printf("[%s] EA=0x%08X, deterministic 2-thread handshake, %d rounds...\n", name, EA_EDGE, T4_ROUNDS);

    vm_commit_page(EA_EDGE);
    memset(vm_base + EA_EDGE, 0, 128);

    g_t4_go = 0; g_t4_done = 0; g_t4_seq = 0; g_t4_stop_writer = 0;
    HANDLE hw = (HANDLE)_beginthreadex(NULL, 0, t4_writer, NULL, 0, NULL);

    spu_context* ctx = &g_spu_pool[1];
    spu_context_init(ctx, 0xB000u);
    int already_failed_success = 0;
    int rounds_ok = 0;

    for (int round = 0; round < T4_ROUNDS; round++) {
        mfc_stage(ctx, 0x1000, EA_EDGE, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD); /* fresh reservation on this line */

        InterlockedExchange(&g_t4_done, 0);
        InterlockedExchange(&g_t4_go, 1);
        int spins = 0;
        while (!g_t4_done) { SwitchToThread(); if (++spins > 5000000) break; }
        if (!g_t4_done) { fail(name, "writer handshake timed out at round %d", round); break; }

        uint8_t* pbuf = ctx->ls + 0x2000;
        memset(pbuf, 0xEE, 128);
        mfc_stage(ctx, 0x2000, EA_EDGE, 128, 0);
        uint32_t stat = mfc_go(ctx, MFC_PUTLLC_CMD);
        if (stat == MFC_PUTLLC_SUCCESS && !already_failed_success) {
            already_failed_success = 1;
            fail(name, "round %d: PUTLLC unexpectedly SUCCEEDED despite the writer's "
                 "committed write strictly happening-before this attempt (handshake-ordered)", round);
        }

        long committed_seq = g_t4_seq;
        mfc_stage(ctx, 0x1000, EA_EDGE, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        uint32_t seen; memcpy(&seen, ctx->ls + 0x1000, 4);
        if ((long)seen != committed_seq) {
            fail(name, "round %d: post-failure GETLLAR on EA=0x%08X returned seq=%u, "
                 "expected the writer's just-committed seq=%ld (stale re-serve / lost wakeup)",
                 round, EA_EDGE, seen, committed_seq);
        } else {
            rounds_ok++;
        }
    }

    InterlockedExchange(&g_t4_stop_writer, 1);
    InterlockedExchange(&g_t4_go, 1); /* wake the writer out of its spin so it can exit */
    WaitForSingleObject(hw, 2000);
    CloseHandle(hw);

    int ok = (g_fail_count == before);
    printf("[%s] rounds_ok=%d/%d -> %s\n", name, rounds_ok, T4_ROUNDS, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 5 -- CAS RETRY LIVENESS
 *
 * Under sustained contention (K writers hammering the SAME line), every
 * writer's GETLLAR/compute/PUTLLC retry loop must eventually commit its full
 * quota of increments within a bounded wall-clock window -- no thread starves
 * forever. Each writer owns a private 4-byte word within the shared 128-byte
 * line (offset = id*4) so successes are externally verifiable (monotonic
 * per-writer counter) without relying on the runtime's own bookkeeping.
 * ========================================================================= */
#define EA_LIVE          0x40104000u
#define T5_WRITERS       6
#define T5_TARGET        300
#define T5_BOUND_MS      15000

typedef struct { int slot; int id; LONG volatile done; LONG volatile retries; } t5_arg;

static unsigned __stdcall t5_writer(void* p)
{
    t5_arg* a = (t5_arg*)p;
    spu_context* ctx = &g_spu_pool[a->slot];
    spu_context_init(ctx, 0xC000u + (uint32_t)a->slot);
    uint32_t rng = g_seed ^ (0x9E3779B9u * (uint32_t)(a->id + 3));

    while (a->done < T5_TARGET) {
        mfc_stage(ctx, 0x1000, EA_LIVE, 128, 0);
        mfc_go(ctx, MFC_GETLLAR_CMD);
        uint8_t* buf = ctx->ls + 0x1000;
        uint32_t mine; memcpy(&mine, buf + a->id * 4, 4);
        mine++;
        memcpy(buf + a->id * 4, &mine, 4);
        mfc_stage(ctx, 0x1000, EA_LIVE, 128, 0);
        uint32_t stat = mfc_go(ctx, MFC_PUTLLC_CMD);
        if (stat == MFC_PUTLLC_SUCCESS) {
            InterlockedIncrement(&a->done);
        } else {
            InterlockedIncrement(&a->retries);
            /* small randomized backoff -- avoids a perfect lockstep livelock
               between identical-speed threads while still keeping pressure high */
            if ((xorshift32(&rng) & 0xF) == 0) SwitchToThread();
        }
    }
    return 0;
}

static int test5_cas_retry_liveness(void)
{
    const char* name = "test5_cas_retry_liveness";
    LONG before = g_fail_count;
    printf("[%s] EA=0x%08X, %d writers, target=%d commits each, bound=%dms...\n",
           name, EA_LIVE, T5_WRITERS, T5_TARGET, T5_BOUND_MS);

    vm_commit_page(EA_LIVE);
    memset(vm_base + EA_LIVE, 0, 128);

    t5_arg a[T5_WRITERS];
    HANDLE h[T5_WRITERS];
    for (int i = 0; i < T5_WRITERS; i++) {
        a[i].slot = i; a[i].id = i; a[i].done = 0; a[i].retries = 0;
        h[i] = (HANDLE)_beginthreadex(NULL, 0, t5_writer, &a[i], 0, NULL);
    }

    DWORD wr = WaitForMultipleObjects(T5_WRITERS, h, TRUE, T5_BOUND_MS);
    int starved = (wr == WAIT_TIMEOUT);
    if (starved) {
        for (int i = 0; i < T5_WRITERS; i++) {
            if (a[i].done < T5_TARGET) {
                fail(name, "writer id=%d starved: only %ld/%d commits after %dms "
                     "(retries=%ld) -- unfair test-and-set spinlock under sustained contention",
                     i, a[i].done, T5_TARGET, T5_BOUND_MS, a[i].retries);
            }
        }
        /* Threads may still be running past the bound; give them a little
         * more time so the process can exit cleanly, then abandon (their
         * target is unmet regardless). */
        WaitForMultipleObjects(T5_WRITERS, h, TRUE, 2000);
    }
    for (int i = 0; i < T5_WRITERS; i++) CloseHandle(h[i]);

    LONG max_retries = 0, min_done = a[0].done;
    for (int i = 0; i < T5_WRITERS; i++) {
        if (a[i].retries > max_retries) max_retries = a[i].retries;
        if (a[i].done < min_done) min_done = a[i].done;
        printf("  writer id=%d: done=%ld/%d retries=%ld\n", i, a[i].done, T5_TARGET, a[i].retries);
    }

    int ok = (g_fail_count == before);
    printf("[%s] min_done=%ld/%d max_retries=%ld -> %s\n", name, min_done, T5_TARGET, max_retries, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char** argv)
{
    g_seed = 0;
    if (argc > 1) g_seed = (uint32_t)strtoul(argv[1], NULL, 10);
    if (g_seed == 0) g_seed = 0x2545F491u;
    printf("=== ps3recomp spu_atomics_stress -- seed=%u ===\n", (unsigned)g_seed);

    vm_init();

    ULONGLONG t_start = now_ms();

    int rc0  = test0_atomic_status_channel();
    int rc1  = test1_reservation_kill();
    int rc1b = test1b_bulk_writer_gap();   /* NOT folded into the pass/fail spec set below */
    int rc2  = test2_snapshot_atomicity();
    int rc3  = test3_fastpath_coherence();
    int rc4  = test4_lost_wakeup_edge();
    int rc5  = test5_cas_retry_liveness();

    ULONGLONG total_ms = now_ms() - t_start;
    printf("=== total wall time: %llums ===\n", (unsigned long long)total_ms);

    printf("=== invariant verdicts ===\n");
    printf("  0 atomic-status lifecycle      : %s\n", rc0  ? "FAIL" : "PASS");
    printf("  1 reservation-kill (ABA)      : %s\n", rc1  ? "FAIL" : "PASS");
    printf("  1b bulk-writer gap (bonus)    : %s\n", rc1b ? "REPRODUCED (pre-existing, documented)" : "not reproduced");
    printf("  2 snapshot atomicity          : %s\n", rc2  ? "FAIL" : "PASS");
    printf("  3 fast-path coherence         : %s\n", rc3  ? "FAIL" : "PASS");
    printf("  4 lost-wakeup edge            : %s\n", rc4  ? "FAIL" : "PASS");
    printf("  5 CAS retry liveness          : %s\n", rc5  ? "FAIL" : "PASS");

    int failed = rc0 || rc1 || rc2 || rc3 || rc4 || rc5;
    printf("=== RESULT: %s (fail_count=%ld; test1b is informational, excluded) ===\n",
           failed ? "FAIL" : "PASS", g_fail_count);
    return failed ? 1 : 0;
}
