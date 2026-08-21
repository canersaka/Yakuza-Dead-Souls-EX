/*
 * ps3recomp - unit tests for the guest page-generation dirty tracker
 *
 * Focus: alignment/clipping behavior, block fast-path correctness, snapshot
 * collect/ack/undo semantics, budget and span-capacity partial collection,
 * counter wrap-around, and the full seqlock contract under real writer
 * concurrency (writers mutate a guest buffer while a mirror thread
 * incrementally copies dirty pages; final states must converge).
 */
#include "../rsx_guest_pages.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

#define PG RSX_GUEST_PAGE_SIZE
#define BLK RSX_GUEST_BLOCK_SIZE

static u32* snap_alloc(const rsx_guest_pages* t, u32 space, u32 off, u32 size)
{
    const u32 len = rsx_guest_pages_snapshot_len(t, space, off, size);
    u32* snap = (u32*)calloc(len ? len : 1, sizeof(u32));
    rsx_guest_pages_snapshot(t, space, off, size, snap);
    return snap;
}

static void test_basic_marking(void)
{
    rsx_guest_pages t;
    CHECK(rsx_guest_pages_init(&t, 8u * BLK, 4u * BLK) == 0, "init");

    u32* snap = snap_alloc(&t, 0, 0, 8u * BLK);
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, 0, 8u * BLK, snap),
          "fresh snapshot is clean");

    /* Aligned single-page write. */
    rsx_guest_pages_note_write(&t, 0, 2u * PG, 16);
    CHECK(rsx_guest_pages_range_dirty(&t, 0, 0, 8u * BLK, snap),
          "write makes the range dirty");
    rsx_guest_span spans[8];
    int more = -1;
    u32 n = rsx_guest_pages_collect_dirty(&t, 0, 0, 8u * BLK, snap,
                                          spans, 8, 0, &more);
    CHECK(n == 1 && spans[0].offset == 2u * PG && spans[0].size == PG,
          "collect returns exactly the written page");
    CHECK(more == 0, "nothing further dirty");
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, 0, 8u * BLK, snap),
          "collect acknowledged the page");

    /* Unaligned cross-page write: last 2 bytes of page 4 + 2 into page 5. */
    rsx_guest_pages_note_write(&t, 0, 5u * PG - 2u, 4);
    n = rsx_guest_pages_collect_dirty(&t, 0, 0, 8u * BLK, snap,
                                      spans, 8, 0, &more);
    CHECK(n == 1 && spans[0].offset == 4u * PG && spans[0].size == 2u * PG,
          "cross-page write dirties and coalesces both pages");

    /* Zero size is a no-op; out-of-space is clipped away. */
    rsx_guest_pages_note_write(&t, 0, 3u * PG, 0);
    rsx_guest_pages_note_write(&t, 0, 9u * BLK, 64);
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, 0, 8u * BLK, snap),
          "zero-size and out-of-range writes mark nothing");

    /* Write clamped at the end of the space. */
    rsx_guest_pages_note_write(&t, 0, 8u * BLK - 8u, 4096);
    n = rsx_guest_pages_collect_dirty(&t, 0, 0, 8u * BLK, snap,
                                      spans, 8, 0, &more);
    CHECK(n == 1 && spans[0].offset == 8u * BLK - PG && spans[0].size == PG,
          "end-of-space write clamps to the last page");

    /* Unaligned bulk write spanning blocks: starts 7 bytes into page 15 and
     * ends 7 bytes into page 49, so 35 whole pages must dirty. */
    rsx_guest_pages_note_write(&t, 0, BLK - PG + 7u, 2u * BLK + 2u * PG);
    n = rsx_guest_pages_collect_dirty(&t, 0, 0, 8u * BLK, snap,
                                      spans, 8, 0, &more);
    CHECK(n == 1 && spans[0].offset == BLK - PG &&
              spans[0].size == 2u * BLK + 3u * PG,
          "bulk write marks every covered page across blocks");

    /* Spaces are independent. */
    u32* msnap = snap_alloc(&t, 1, 0, 4u * BLK);
    rsx_guest_pages_note_write(&t, 1, 0, 32);
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, 0, 8u * BLK, snap),
          "main-space write leaves local clean");
    CHECK(rsx_guest_pages_range_dirty(&t, 1, 0, 4u * BLK, msnap),
          "main-space write dirties main");
    CHECK(rsx_guest_pages_space_epoch(&t, 1) > 0, "epoch advanced");

    free(msnap);
    free(snap);
    rsx_guest_pages_destroy(&t);
}

static void test_block_fast_path(void)
{
    rsx_guest_pages t;
    CHECK(rsx_guest_pages_init(&t, 4u * BLK, 0) == 0, "init");

    /* Range = pages 0..1 of block 1.  A write elsewhere in block 1 changes
     * the block counter but not our pages: the query must fall through the
     * block level and still report clean. */
    const u32 off = BLK, size = 2u * PG;
    u32* snap = snap_alloc(&t, 0, off, size);
    rsx_guest_pages_note_write(&t, 0, BLK + 8u * PG, 64);
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, off, size, snap),
          "neighbor-page write in the same block stays clean");
    rsx_guest_pages_note_write(&t, 0, BLK + PG + 4u, 8);
    CHECK(rsx_guest_pages_range_dirty(&t, 0, off, size, snap),
          "in-range page write is detected after a block-level miss");

    /* Zero-size space: everything reports clean/no-op. */
    CHECK(rsx_guest_pages_snapshot_len(&t, 1, 0, BLK) == 0,
          "disabled space has no snapshot");
    rsx_guest_pages_note_write(&t, 1, 0, 64);
    CHECK(rsx_guest_pages_space_epoch(&t, 1) == 0, "disabled space is inert");

    free(snap);
    rsx_guest_pages_destroy(&t);
}

static void test_budget_and_span_capacity(void)
{
    rsx_guest_pages t;
    CHECK(rsx_guest_pages_init(&t, 4u * BLK, 0) == 0, "init");
    u32* snap = snap_alloc(&t, 0, 0, 4u * BLK);

    /* Dirty 8 alternating pages (never coalescable). */
    for (u32 i = 0; i < 8; i++)
        rsx_guest_pages_note_write(&t, 0, (2u * i) * PG, 1);

    rsx_guest_span spans[16];
    int more = 0;
    u32 n = rsx_guest_pages_collect_dirty(&t, 0, 0, 4u * BLK, snap,
                                          spans, 16, 3u * PG, &more);
    CHECK(n == 3 && more == 1, "byte budget stops collection");
    u32 total = 0;
    for (u32 i = 0; i < n; i++) total += spans[i].size;
    CHECK(total == 3u * PG, "budget respected exactly on page boundaries");

    n = rsx_guest_pages_collect_dirty(&t, 0, 0, 4u * BLK, snap,
                                      spans, 2, 0, &more);
    CHECK(n == 2 && more == 1, "span capacity stops collection");

    u32 drained = 0;
    for (u32 guard = 0; guard < 8 && more; guard++) {
        n = rsx_guest_pages_collect_dirty(&t, 0, 0, 4u * BLK, snap,
                                          spans, 16, 0, &more);
        drained += n;
    }
    CHECK(more == 0, "repeated collects drain the backlog");
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, 0, 4u * BLK, snap),
          "fully drained");
    CHECK(drained == 3, "remaining alternating pages arrive as spans");

    /* Undo: refuse one uploaded span and observe it dirty again. */
    rsx_guest_pages_note_write(&t, 0, 6u * PG, 1);
    n = rsx_guest_pages_collect_dirty(&t, 0, 0, 4u * BLK, snap,
                                      spans, 16, 0, &more);
    CHECK(n == 1 && spans[0].offset == 6u * PG, "collected the retry page");
    rsx_guest_pages_snapshot_undo(&t, 0, 0, 4u * BLK, snap,
                                  spans[0].offset, spans[0].size);
    CHECK(rsx_guest_pages_range_dirty(&t, 0, 0, 4u * BLK, snap),
          "undo makes the span dirty again");
    n = rsx_guest_pages_collect_dirty(&t, 0, 0, 4u * BLK, snap,
                                      spans, 16, 0, &more);
    CHECK(n == 1 && spans[0].offset == 6u * PG && more == 0,
          "undone span is re-collected");

    free(snap);
    rsx_guest_pages_destroy(&t);
}

static void test_generation_rollover(void)
{
    rsx_guest_pages t;
    CHECK(rsx_guest_pages_init(&t, 2u * BLK, 0) == 0, "init");

    rsx_guest_pages_debug_set_page_gen(&t, 0, 3, 0xFFFFFFFFu);
    u32* snap = snap_alloc(&t, 0, 0, 2u * BLK);
    CHECK(!rsx_guest_pages_range_dirty(&t, 0, 0, 2u * BLK, snap),
          "snapshot at the pre-wrap value is clean");

    rsx_guest_pages_note_write(&t, 0, 3u * PG, 1);   /* 0xFFFFFFFF -> 0 */
    CHECK(rsx_guest_pages_page_gen(&t, 0, 3) == 0, "counter wrapped to zero");
    CHECK(rsx_guest_pages_range_dirty(&t, 0, 0, 2u * BLK, snap),
          "wrap-around write is detected (equality compare)");

    rsx_guest_span span;
    int more;
    u32 n = rsx_guest_pages_collect_dirty(&t, 0, 0, 2u * BLK, snap,
                                          &span, 1, 0, &more);
    CHECK(n == 1 && span.offset == 3u * PG, "wrapped page collected");
    rsx_guest_pages_note_write(&t, 0, 3u * PG, 1);   /* 0 -> 1 */
    CHECK(rsx_guest_pages_range_dirty(&t, 0, 0, 2u * BLK, snap),
          "post-wrap writes keep tracking");

    free(snap);
    rsx_guest_pages_destroy(&t);
}

/* ---- concurrency: writers vs an incremental mirror ---------------------- */

#define CONC_SPACE_SIZE (8u * BLK)          /* 512 KiB, 128 pages */
#define CONC_WRITERS    4
#define CONC_ITERS      20000

typedef struct conc_shared {
    rsx_guest_pages tracker;
    u8* guest;
    u8* mirror;
    volatile LONG writers_done;
} conc_shared;

typedef struct conc_writer_arg {
    conc_shared* sh;
    u32 seed;
} conc_writer_arg;

static u32 xorshift(u32* s)
{
    u32 x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0x9E3779B9u;
    return *s;
}

static unsigned __stdcall conc_writer(void* p)
{
    conc_writer_arg* a = (conc_writer_arg*)p;
    u32 rng = a->seed;
    for (u32 i = 0; i < CONC_ITERS; i++) {
        const u32 size = 1u + (xorshift(&rng) & 0x1FFu);         /* 1..512 */
        const u32 offset = xorshift(&rng) % (CONC_SPACE_SIZE - size);
        const u8 value = (u8)xorshift(&rng);
        memset(a->sh->guest + offset, value, size);
        rsx_guest_pages_note_write(&a->sh->tracker, 0, offset, size);
    }
    InterlockedIncrement(&a->sh->writers_done);
    return 0;
}

static void conc_mirror_pass(conc_shared* sh, u32* snap)
{
    rsx_guest_span spans[32];
    int more = 1;
    while (more) {
        const u32 n = rsx_guest_pages_collect_dirty(
            &sh->tracker, 0, 0, CONC_SPACE_SIZE, snap, spans, 32, 0, &more);
        /* Payload copy strictly AFTER the generation reads (contract). */
        for (u32 i = 0; i < n; i++)
            memcpy(sh->mirror + spans[i].offset, sh->guest + spans[i].offset,
                   spans[i].size);
        if (!n && more)
            break;   /* nothing emitted; avoid a hot spin on a live writer */
    }
}

static void test_concurrent_writers(void)
{
    conc_shared sh;
    memset(&sh, 0, sizeof(sh));
    CHECK(rsx_guest_pages_init(&sh.tracker, CONC_SPACE_SIZE, 0) == 0, "init");
    sh.guest = (u8*)calloc(1, CONC_SPACE_SIZE);
    sh.mirror = (u8*)calloc(1, CONC_SPACE_SIZE);

    u32* snap = snap_alloc(&sh.tracker, 0, 0, CONC_SPACE_SIZE);

    conc_writer_arg args[CONC_WRITERS];
    HANDLE threads[CONC_WRITERS];
    for (u32 i = 0; i < CONC_WRITERS; i++) {
        args[i].sh = &sh;
        args[i].seed = 0xC0FFEE01u + i * 7919u;
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, conc_writer,
                                            &args[i], 0, NULL);
    }

    /* Incremental mirroring while the writers are live. */
    while (sh.writers_done != CONC_WRITERS) {
        conc_mirror_pass(&sh, snap);
        Sleep(1);
    }
    WaitForMultipleObjects(CONC_WRITERS, threads, TRUE, INFINITE);
    for (u32 i = 0; i < CONC_WRITERS; i++)
        CloseHandle(threads[i]);

    /* Final drain: every published write must now be visible and copied. */
    conc_mirror_pass(&sh, snap);
    CHECK(!rsx_guest_pages_range_dirty(&sh.tracker, 0, 0, CONC_SPACE_SIZE,
                                       snap),
          "post-join drain leaves the range clean");
    CHECK(memcmp(sh.guest, sh.mirror, CONC_SPACE_SIZE) == 0,
          "mirror converged to the guest content");

    free(snap);
    free(sh.guest);
    free(sh.mirror);
    rsx_guest_pages_destroy(&sh.tracker);
}

int main(void)
{
    test_basic_marking();
    test_block_fast_path();
    test_budget_and_span_capacity();
    test_generation_rollover();
    test_concurrent_writers();
    if (failures) {
        fprintf(stderr, "test_guest_pages: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_guest_pages: ALL PASS\n");
    return 0;
}
