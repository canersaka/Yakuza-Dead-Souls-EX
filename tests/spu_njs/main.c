/* spu_njs conformance + semantics tests.
 *
 * Pins the native-job-scheduler foundation to the live jobchain contract:
 *   1) descriptor parse acceptance/rejection exactly mirrors run_job
 *      (libs/spurs/cellSpurs.c, rules cited in spu_njs.c);
 *   2) local-store construction goldens: layout cursor arithmetic,
 *      sub-quadword low-nibble placement, context block, SPU ABI stack,
 *      register spec, slot fit fallback -- all independently re-derived
 *      here from the cited rules (not read back from the implementation);
 *   3) scheduler semantics: barrier ordering, abort/end/terminal behavior,
 *      sequential determinism, concurrent drain.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "spu_njs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int g_fail = 0;
#define CHECK(cond, ...) do {                                            \
    if (!(cond)) {                                                       \
        g_fail++;                                                        \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
        fprintf(stderr, __VA_ARGS__);                                    \
        fprintf(stderr, "\n");                                           \
    }                                                                    \
} while (0)

/* big-endian writers for descriptor construction */
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void w64(uint8_t* p, uint64_t v) { w32(p, (uint32_t)(v >> 32)); w32(p + 4, (uint32_t)v); }
static uint32_t r32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- synthetic guest memory ------------------------------------------- */
#define GUEST_SIZE (2u << 20)
static uint8_t g_guest[GUEST_SIZE];

static int guest_read(void* user, uint32_t ea, void* dst, uint32_t size)
{
    (void)user;
    if (ea >= GUEST_SIZE || size > GUEST_SIZE - ea)
        return 0;
    memcpy(dst, g_guest + ea, size);
    return 1;
}

/* base legacy descriptor: bin at 0x10000/0x40 bytes, no lists */
static uint32_t make_desc(uint8_t* d, uint32_t size)
{
    memset(d, 0, 0x400);
    w64(d + 0x00, 0x10000);              /* eaBinary */
    w16(d + 0x08, 0x40 / 16);            /* sizeBinary in qwords */
    w16(d + 0x0a, 0);                    /* sizeDmaList */
    w32(d + 0x10, 1);                    /* useInOutBuffer kind */
    w32(d + 0x14, 0);                    /* sizeInOrInOut */
    w32(d + 0x18, 0);                    /* sizeOut */
    w16(d + 0x1c, 0);                    /* sizeStack -> default 8192 */
    w16(d + 0x1e, 0);                    /* sizeScratch */
    w32(d + 0x24, 0);                    /* sizeCacheDmaList */
    return size;
}

static void test_parse(void)
{
    uint8_t d[0x400];
    spu_njs_job j;

    make_desc(d, 0x40);
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "baseline descriptor must parse");
    CHECK(j.bin_ea == 0x10000 && j.bin_size == 0x40, "bin fields");
    CHECK(j.size_stack == 8192, "default stack 8192");

    CHECK(spu_njs_parse_descriptor(d, 0x2f, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "size < 0x30 rejected");
    CHECK(spu_njs_parse_descriptor(d, 0x401, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "size > 0x400 rejected");
    CHECK(spu_njs_parse_descriptor(d, 0x44, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "unaligned size rejected");
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0xfffffff4, &j) ==
          SPU_NJS_E_DESCRIPTOR, "ea > 0xFFFFFFF0 rejected");

    make_desc(d, 0x40);
    d[0x2c] = 4;                          /* BINARY2 jobType bit */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) ==
          SPU_NJS_E_BINARY2, "BINARY2 refused honestly");

    make_desc(d, 0x40);
    d[0x2c] = 1;                          /* STALL_SUCCESSOR */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "STALL_SUCCESSOR is not BINARY2");
    d[0x2c] = 2;                          /* MEMORY_CHECK */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "MEMORY_CHECK is not BINARY2");

    make_desc(d, 0x40);
    w64(d + 0x00, 0);                     /* no binary */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) ==
          SPU_NJS_E_INVALID_BIN, "missing binary rejected");

    make_desc(d, 0x40);
    w32(d + 0x24, 5 * 8);                 /* > 4 cache entries */
    CHECK(spu_njs_parse_descriptor(d, 0x80, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "cache list > 4 rejected");

    /* io item rules */
    make_desc(d, 0x40);
    w16(d + 0x0a, 8);
    w32(d + 0x14, 0x100);
    w64(d + 0x30, ((uint64_t)0x4010 << 32) | 0x20000);  /* size > 0x4000 */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "io size > 0x4000 rejected");
    w64(d + 0x30, ((uint64_t)0x00028000ull << 32) | 0x20000); /* reserved bit */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "reserved flag bits rejected");
    w64(d + 0x30, ((uint64_t)3 << 32) | 0x20000);       /* sub-16 size 3 */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "sub-quadword size 3 rejected");
    w64(d + 0x30, ((uint64_t)16 << 32) | 0x20004);      /* 16B item, ea&15 */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) ==
          SPU_NJS_E_DESCRIPTOR, "misaligned 16B element rejected");
    w64(d + 0x30, ((uint64_t)0x80000000ull << 32) | 0); /* stall on 0-size */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "stall-and-notify flag on zero-length element accepted");
    CHECK(j.io_count == 1 && j.io[0].stall_notify == 1, "stall flag typed");

    /* cache size rules (cache list starts at d+0x30+dma_list_size; dma
     * list is empty here) */
    make_desc(d, 0x40);
    w32(d + 0x24, 8);
    w64(d + 0x30, ((uint64_t)0x18 << 32) | 0x30000);    /* size & 15 */
    CHECK(spu_njs_parse_descriptor(d, 0x80, 0x40000000, &j) ==
          SPU_NJS_E_INVALID_BIN, "cache size &15 rejected");
}

static void test_build_golden(void)
{
    uint8_t d[0x400];
    make_desc(d, 0x40);
    /* io list: one 16B element at 0x20000, one 4B element at 0x20034
     * (low nibble 4), one stall-notify zero terminator */
    w16(d + 0x0a, 24);
    w32(d + 0x14, 0x40);                  /* size_io */
    w32(d + 0x18, 0x20);                  /* size_out */
    w16(d + 0x1e, 2);                     /* scratch 32 */
    w64(d + 0x30, ((uint64_t)16 << 32) | 0x20000);
    w64(d + 0x38, ((uint64_t)4 << 32) | 0x20034);
    w64(d + 0x40, ((uint64_t)0x80000000ull << 32) | 0x1);
    /* cache list: one 0x20-byte element at 0x21000 */
    w32(d + 0x24, 8);
    w64(d + 0x48, ((uint64_t)0x20 << 32) | 0x21000);

    for (uint32_t i = 0; i < 0x100; ++i) {
        g_guest[0x10000 + i] = (uint8_t)(0xB0 + i);     /* binary */
        g_guest[0x20000 + i] = (uint8_t)(0x40 + i);     /* io source */
        g_guest[0x21000 + i] = (uint8_t)(0x80 + i);     /* cache source */
    }

    spu_njs_job j;
    CHECK(spu_njs_parse_descriptor(d, 0x80, 0x40001000, &j) == SPU_NJS_OK,
          "golden descriptor parses");
    static uint8_t ls[256 * 1024];
    memset(ls, 0xCC, sizeof ls);
    const uint32_t tags[2] = {0x11, 0x17};
    spu_njs_ls_init init;
    CHECK(spu_njs_build_ls(&j, ls, 0x4c00, tags, 1, guest_read, NULL,
                           &init) == SPU_NJS_OK, "golden build");

    /* independently derived layout: slot 0x4C00, bin 0x40 ->
     * cursor (0x4C40+1023)&~1023 = 0x5000; io 0x40 @0x5000 -> 0x5040;
     * out 0x20 @(align 0x5040->0x5400)... alignment 1024: 0x5040 -> 0x5400;
     * cache 0x20 @0x5800; scratch+stack (0x20+0x2000) @0x5C00. */
    CHECK(init.slot == 0x4c00, "slot preferred");
    CHECK(init.io_ls == 0x5000, "io_ls 0x5000 got 0x%X", init.io_ls);
    CHECK(init.out_ls == 0x5400, "out_ls 0x5400 got 0x%X", init.out_ls);
    CHECK(init.cache_ls[0] == 0x5800, "cache_ls 0x5800 got 0x%X",
          init.cache_ls[0]);
    CHECK(init.scratch_stack_ls == 0x5c00, "scratch 0x5C00 got 0x%X",
          init.scratch_stack_ls);
    CHECK(init.pc == 0x4c00 && init.r0 == 0x0a70 &&
          init.r3 == 0x4940 && init.r4 == 0x3f000, "register spec");
    /* stack: end = 0x5C00+0x20+0x2000 = 0x7C20; sp = end-0x30 = 0x7BF0;
     * root = end-0x10 = 0x7C10; ls[sp] = root, ls[root] = 0 */
    CHECK(init.r1_sp == 0x7bf0 && init.r1_avail == 0x2000, "ABI stack spec");
    CHECK(r32(ls + 0x7bf0) == 0x7c10 && r32(ls + 0x7c10) == 0,
          "ABI frame words");
    /* binary bytes at slot */
    CHECK(ls[0x4c00] == 0xB0 && ls[0x4c3f] == (uint8_t)(0xB0 + 0x3f),
          "binary copied");
    /* io: 16B element at io_ls+0; 4B element in the SECOND 16B slot at
     * low-nibble offset 4 (run_job sub-quadword placement) */
    CHECK(ls[0x5000] == 0x40 && ls[0x500f] == 0x4f, "io element 0");
    CHECK(ls[0x5010 + 4] == g_guest[0x20034] &&
          ls[0x5010 + 7] == g_guest[0x20037], "sub-quadword nibble placement");
    /* cache bytes */
    CHECK(ls[0x5800] == 0x80 && ls[0x581f] == 0x9f, "cache copied");
    /* context block (big-endian words at 0x4940) */
    CHECK(r32(ls + 0x4940 + 0x00) == 0x5000, "ctx io_ls");
    CHECK(r32(ls + 0x4940 + 0x04) == 0x5800, "ctx cache0");
    CHECK(r32(ls + 0x4940 + 0x14) == ((0x80u / 128u) << 28), "ctx flags");
    CHECK(r32(ls + 0x4940 + 0x1c) == 0x5400, "ctx out_ls");
    CHECK(r32(ls + 0x4940 + 0x20) == 0x5c00, "ctx scratch");
    CHECK(r32(ls + 0x4940 + 0x24) == 0x17, "ctx dma tag (parity 1)");
    CHECK(r32(ls + 0x4940 + 0x28) == 0 && r32(ls + 0x4940 + 0x2c) == 0x40001000,
          "ctx descriptor ea");
    /* descriptor copy */
    CHECK(memcmp(ls + 0x3f000, d, 0x80) == 0, "descriptor copied");
}

static void test_build_fallback(void)
{
    uint8_t d[0x400];
    make_desc(d, 0x40);
    w32(d + 0x14, 0x31000);               /* io too big for slot 0xE400 */
    spu_njs_job j;
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "fallback descriptor parses");
    static uint8_t ls[256 * 1024];
    const uint32_t tags[2] = {1, 2};
    spu_njs_ls_init init;
    CHECK(spu_njs_build_ls(&j, ls, 0xe400, tags, 0, guest_read, NULL,
                           &init) == SPU_NJS_OK, "fallback build ok");
    CHECK(init.slot == 0x4c00, "fell back to 0x4C00, got 0x%X", init.slot);

    w32(d + 0x14, 0x3b000);               /* fits neither slot */
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "nomem descriptor parses");
    CHECK(spu_njs_build_ls(&j, ls, 0xe400, tags, 0, guest_read, NULL,
                           &init) == SPU_NJS_E_NOMEM, "NOMEM on no slot fit");
}

/* ---- scheduler semantics ---------------------------------------------- */

#define REC_MAX 4096
static struct { uint64_t seq[REC_MAX]; unsigned n; int slow; } g_rec;
#if defined(_WIN32)
static SRWLOCK g_rec_lock = SRWLOCK_INIT;
#endif

static int rec_exec(void* user, const spu_njs_job* job)
{
    (void)user;
    if (g_rec.slow) {
#if defined(_WIN32)
        Sleep(2);
#endif
    }
#if defined(_WIN32)
    AcquireSRWLockExclusive(&g_rec_lock);
#endif
    if (g_rec.n < REC_MAX)
        g_rec.seq[g_rec.n++] = job->sequence;
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&g_rec_lock);
#endif
    return 1;
}

static void submit_n(spu_njs_sched* s, spu_njs_job* j, int n)
{
    for (int i = 0; i < n; ++i)
        CHECK(spu_njs_submit(s, j) == SPU_NJS_OK, "submit");
}

static void test_sched(void)
{
    uint8_t d[0x400];
    make_desc(d, 0x40);
    spu_njs_job j;
    CHECK(spu_njs_parse_descriptor(d, 0x40, 0x40000000, &j) == SPU_NJS_OK,
          "sched job parses");

    /* sequential determinism: completion order == submission order */
    memset(&g_rec, 0, sizeof g_rec);
    spu_njs_sched* s = spu_njs_sched_create(1, rec_exec, NULL, NULL);
    submit_n(s, &j, 24);
    CHECK(spu_njs_sync(s) == SPU_NJS_OK, "sync");
    CHECK(spu_njs_end(s) == SPU_NJS_OK, "end");
    CHECK(spu_njs_terminal(s) == 1, "terminal end");
    CHECK(spu_njs_submit(s, &j) == SPU_NJS_E_STATE, "submit after end");
    CHECK(g_rec.n == 24, "24 completions, got %u", g_rec.n);
    for (unsigned i = 0; i < g_rec.n; ++i)
        CHECK(g_rec.seq[i] == i, "sequential order @%u", i);
    spu_njs_sched_destroy(s);

    /* concurrent: barrier ordering across SYNC windows */
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.slow = 1;
    s = spu_njs_sched_create(4, rec_exec, NULL, NULL);
    submit_n(s, &j, 12);                  /* window A: seq 0..11 */
    CHECK(spu_njs_sync(s) == SPU_NJS_OK, "sync A");
    unsigned nA = g_rec.n;
    CHECK(nA == 12, "sync waited for window A (%u)", nA);
    submit_n(s, &j, 12);                  /* window B: seq 12..23 */
    CHECK(spu_njs_end(s) == SPU_NJS_OK, "end drains B");
    CHECK(g_rec.n == 24, "all complete at end");
    for (unsigned i = 0; i < 12; ++i)
        CHECK(g_rec.seq[i] < 12, "window A completion before barrier @%u", i);
    for (unsigned i = 12; i < 24; ++i)
        CHECK(g_rec.seq[i] >= 12, "window B completion after barrier @%u", i);
    CHECK(spu_njs_completed(s) == 24 && spu_njs_submitted(s) == 24,
          "counters");
    spu_njs_sched_destroy(s);

    /* abort: no further submissions; in-flight drains; terminal 2 */
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.slow = 1;
    s = spu_njs_sched_create(4, rec_exec, NULL, NULL);
    submit_n(s, &j, 8);
    CHECK(spu_njs_abort(s) == SPU_NJS_OK, "abort");
    CHECK(spu_njs_terminal(s) == 2, "terminal aborted");
    CHECK(spu_njs_submit(s, &j) == SPU_NJS_E_STATE, "submit after abort");
    CHECK(spu_njs_sync(s) == SPU_NJS_E_STATE, "sync after abort");
    CHECK(spu_njs_completed(s) <= 8, "no phantom completions");
    spu_njs_sched_destroy(s);
}

int main(void)
{
    test_parse();
    test_build_golden();
    test_build_fallback();
    test_sched();
    if (g_fail) {
        fprintf(stderr, "spu_njs: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("spu_njs: OK (parse conformance, LS goldens, slot fallback, "
           "scheduler semantics)\n");
    return 0;
}
