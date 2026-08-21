/*
 * ps3recomp - unit tests for the persistent GPU mirror core
 *
 * A mock backend records uploads into a shadow "GPU" buffer, so every
 * registration/dirty/budget/reject/lifetime behavior is validated offline
 * byte-for-byte, with upload-call accounting proving that unchanged
 * resources are reused rather than re-uploaded.
 */
#include "../rsx_gpu_mirror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

#define PG RSX_GUEST_PAGE_SIZE
#define BLK RSX_GUEST_BLOCK_SIZE
#define LOCAL_SIZE (4u * BLK)
#define MAIN_SIZE  (2u * BLK)

typedef struct mock_backend {
    u8 guest[2][4u * BLK];
    u8 gpu[2][4u * BLK];
    u32 space_size[2];
    u32 uploads;
    u32 upload_bytes;
    u32 fail_next;         /* reject this many upcoming uploads */
} mock_backend;

static const u8* mock_guest_ptr(void* user, u32 space, u32 offset,
                                u32 min_bytes)
{
    mock_backend* mb = (mock_backend*)user;
    if (space >= 2 || (u64)offset + min_bytes > mb->space_size[space])
        return NULL;
    return mb->guest[space] + offset;
}

static int mock_upload(void* user, u32 space, u32 offset, const u8* src,
                       u32 size)
{
    mock_backend* mb = (mock_backend*)user;
    if (mb->fail_next) {
        mb->fail_next--;
        return -1;
    }
    memcpy(mb->gpu[space] + offset, src, size);
    mb->uploads++;
    mb->upload_bytes += size;
    return 0;
}

typedef struct fixture {
    rsx_guest_pages tracker;
    mock_backend mb;
    rsx_gpu_mirror* m;
} fixture;

static void fixture_up(fixture* f)
{
    memset(f, 0, sizeof(*f));
    f->mb.space_size[0] = LOCAL_SIZE;
    f->mb.space_size[1] = MAIN_SIZE;
    for (u32 s = 0; s < 2; s++)
        for (u32 i = 0; i < f->mb.space_size[s]; i++)
            f->mb.guest[s][i] = (u8)(i * 7u + s * 13u + 1u);
    CHECK(rsx_guest_pages_init(&f->tracker, LOCAL_SIZE, MAIN_SIZE) == 0,
          "tracker init");
    rsx_gpu_mirror_ops ops = {&f->mb, mock_guest_ptr, mock_upload};
    f->m = rsx_gpu_mirror_create(&f->tracker, &ops);
    CHECK(f->m != NULL, "mirror create");
}

static void fixture_down(fixture* f)
{
    rsx_gpu_mirror_destroy(f->m);
    rsx_guest_pages_destroy(&f->tracker);
}

/* Write `size` fresh bytes at guest offset and publish it. */
static void guest_write(fixture* f, u32 space, u32 offset, u32 size, u8 seed)
{
    for (u32 i = 0; i < size; i++)
        f->mb.guest[space][offset + i] = (u8)(seed + i);
    rsx_guest_pages_note_write(&f->tracker, space, offset, size);
}

static int gpu_matches(fixture* f, u32 space, u32 offset, u32 size)
{
    return memcmp(f->mb.gpu[space] + offset, f->mb.guest[space] + offset,
                  size) == 0;
}

static void test_register_sync_reuse(void)
{
    static fixture f;
    fixture_up(&f);

    /* Unaligned range: pages must cover it fully. */
    const u32 off = 3u * PG + 100u, size = 2u * PG + 50u;
    rsx_gpu_mirror_range r = rsx_gpu_mirror_register(f.m, 0, off, size);
    CHECK(r != 0, "register");
    CHECK(!rsx_gpu_mirror_range_current(f.m, r), "fresh range is stale");

    u32 up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == 3u * PG, "first sync uploads the 3 covering pages");
    CHECK(gpu_matches(&f, 0, 3u * PG, 3u * PG), "page-aligned content");
    CHECK(rsx_gpu_mirror_range_current(f.m, r), "range current after sync");

    /* Nothing changed: the whole space must skip on the epoch. */
    const u32 uploads_before = f.mb.uploads;
    up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == 0 && f.mb.uploads == uploads_before,
          "unchanged resource re-used, zero uploads");
    rsx_gpu_mirror_stats st;
    rsx_gpu_mirror_get_stats(f.m, &st);
    CHECK(st.epoch_skips >= 2, "clean spaces skip via epoch");

    /* One-byte write re-uploads exactly one page. */
    guest_write(&f, 0, 4u * PG + 5u, 1, 0xA0);
    CHECK(!rsx_gpu_mirror_range_current(f.m, r), "write invalidates range");
    up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == PG, "single dirty page re-uploaded");
    CHECK(gpu_matches(&f, 0, 4u * PG, PG), "re-upload content");
    CHECK(rsx_gpu_mirror_range_current(f.m, r), "current again");

    /* Registration outside the space and empty ranges are rejected. */
    CHECK(rsx_gpu_mirror_register(f.m, 0, LOCAL_SIZE - 4u, 8u) == 0,
          "out-of-space registration rejected");
    CHECK(rsx_gpu_mirror_register(f.m, 0, 0, 0) == 0,
          "empty registration rejected");
    CHECK(rsx_gpu_mirror_register(f.m, 2, 0, PG) == 0,
          "bad space rejected");

    fixture_down(&f);
}

static void test_disjoint_and_overlap(void)
{
    static fixture f;
    fixture_up(&f);

    rsx_gpu_mirror_range a = rsx_gpu_mirror_register(f.m, 0, 0, 2u * PG);
    rsx_gpu_mirror_range b =
        rsx_gpu_mirror_register(f.m, 0, 8u * PG, 2u * PG);
    /* c overlaps a's second page. */
    rsx_gpu_mirror_range c = rsx_gpu_mirror_register(f.m, 0, PG, 2u * PG);
    rsx_gpu_mirror_sync(f.m, 0);
    CHECK(rsx_gpu_mirror_range_current(f.m, a) &&
              rsx_gpu_mirror_range_current(f.m, b) &&
              rsx_gpu_mirror_range_current(f.m, c),
          "all ranges current");

    /* Write into a's exclusive page: b stays current without upload. */
    const u32 uploads0 = f.mb.uploads;
    guest_write(&f, 0, 16, 8, 0x11);
    CHECK(rsx_gpu_mirror_range_current(f.m, b), "b unaffected by a's write");
    u32 up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == PG && f.mb.uploads == uploads0 + 1u,
          "only a's page uploads");

    /* Write into the a/c shared page: exactly one upload serves both. */
    const u32 uploads1 = f.mb.uploads;
    guest_write(&f, 0, PG + 32u, 8, 0x22);
    CHECK(!rsx_gpu_mirror_range_current(f.m, a) &&
              !rsx_gpu_mirror_range_current(f.m, c),
          "shared page invalidates both");
    up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == PG && f.mb.uploads == uploads1 + 1u,
          "shared page uploaded once");
    CHECK(rsx_gpu_mirror_range_current(f.m, a) &&
              rsx_gpu_mirror_range_current(f.m, c),
          "both ranges current after the shared upload");

    /* Main space is independent. */
    rsx_gpu_mirror_range d = rsx_gpu_mirror_register(f.m, 1, PG, PG);
    up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == PG && gpu_matches(&f, 1, PG, PG), "main-space upload");
    CHECK(rsx_gpu_mirror_range_current(f.m, d), "main range current");

    fixture_down(&f);
}

static void test_budget_and_reject(void)
{
    static fixture f;
    fixture_up(&f);

    rsx_gpu_mirror_range r = rsx_gpu_mirror_register(f.m, 0, 0, 8u * PG);
    u32 up = rsx_gpu_mirror_sync(f.m, 3u * PG);
    CHECK(up == 3u * PG, "budget caps the sync");
    CHECK(!rsx_gpu_mirror_range_current(f.m, r), "partially uploaded");
    up = rsx_gpu_mirror_sync(f.m, 3u * PG);
    up += rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == 5u * PG, "later syncs drain the remainder");
    CHECK(rsx_gpu_mirror_range_current(f.m, r), "complete after drain");
    CHECK(gpu_matches(&f, 0, 0, 8u * PG), "content correct across budgets");

    /* Backend rejection: pages stay dirty and retry. */
    guest_write(&f, 0, 2u * PG, 64, 0x33);
    f.mb.fail_next = 1;
    up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == 0, "rejected upload transfers nothing");
    rsx_gpu_mirror_stats st;
    rsx_gpu_mirror_get_stats(f.m, &st);
    CHECK(st.upload_rejects == 1 && st.deferred_syncs >= 1,
          "reject recorded and sync deferred");
    CHECK(!rsx_gpu_mirror_range_current(f.m, r), "range stays stale");
    up = rsx_gpu_mirror_sync(f.m, 0);
    CHECK(up == PG && rsx_gpu_mirror_range_current(f.m, r),
          "retry succeeds");
    CHECK(gpu_matches(&f, 0, 2u * PG, PG), "retried content correct");

    fixture_down(&f);
}

static void test_lifetime(void)
{
    static fixture f;
    fixture_up(&f);

    rsx_gpu_mirror_range r = rsx_gpu_mirror_register(f.m, 0, 0, PG);
    rsx_gpu_mirror_sync(f.m, 0);
    CHECK(rsx_gpu_mirror_unregister(f.m, r) == 1, "unregister");
    CHECK(rsx_gpu_mirror_unregister(f.m, r) == 0, "stale handle rejected");
    CHECK(!rsx_gpu_mirror_range_current(f.m, r), "stale handle not current");

    /* Writes to unregistered memory cause no uploads. */
    const u32 uploads0 = f.mb.uploads;
    guest_write(&f, 0, 8, 8, 0x44);
    CHECK(rsx_gpu_mirror_sync(f.m, 0) == 0 && f.mb.uploads == uploads0,
          "unregistered pages are not maintained");

    /* Re-registration forces a fresh upload even without new writes
     * (the mirror may have missed writes while unmaintained). */
    rsx_gpu_mirror_range r2 = rsx_gpu_mirror_register(f.m, 0, 0, PG);
    CHECK(r2 != 0 && r2 != r, "recycled slot gets a distinct handle");
    CHECK(!rsx_gpu_mirror_range_current(f.m, r2), "re-registered is stale");
    CHECK(rsx_gpu_mirror_sync(f.m, 0) == PG, "re-upload after re-register");
    CHECK(gpu_matches(&f, 0, 0, PG), "re-upload picked up missed write");
    CHECK(rsx_gpu_mirror_range_current(f.m, r2) &&
              !rsx_gpu_mirror_range_current(f.m, r),
          "only the new handle is current");

    /* Many registrations exercise slot growth. */
    rsx_gpu_mirror_range handles[64];
    for (u32 i = 0; i < 64; i++) {
        handles[i] = rsx_gpu_mirror_register(f.m, 0, (i % 32u) * PG, PG);
        CHECK(handles[i] != 0, "bulk registration");
    }
    rsx_gpu_mirror_sync(f.m, 0);
    for (u32 i = 0; i < 64; i++)
        CHECK(rsx_gpu_mirror_unregister(f.m, handles[i]) == 1,
              "bulk unregister");

    fixture_down(&f);
}

int main(void)
{
    test_register_sync_reuse();
    test_disjoint_and_overlap();
    test_budget_and_reject();
    test_lifetime();
    if (failures) {
        fprintf(stderr, "test_gpu_mirror: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gpu_mirror: ALL PASS\n");
    return 0;
}
