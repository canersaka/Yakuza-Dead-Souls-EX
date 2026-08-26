#include "rsx_nr_report_scoreboard.h"
#include "ppu_guest_read.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x, ...) do { if (!(x)) { \
    fprintf(stderr, "FAIL:%d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; \
} } while (0)

typedef struct fixture {
    rsx_nr_report_scoreboard sb;
    u64 clock;
    u64 completed;
    u64 submit_calls;
    u64 publish_calls;
    u32 memory[0x1000 / 4];
    u64 published_sequence[8192];
} fixture;

static u32 read_watch_hits;
static u32 read_watch_ea;
static u32 read_watch_size;
static u32 read_watch_source;

static void test_read_watch(void* user, u32 ea, u32 size, u32 source)
{
    (void)user;
    read_watch_hits++;
    read_watch_ea = ea;
    read_watch_size = size;
    read_watch_source = source;
}

static u64 test_timestamp(void* user)
{
    fixture* f = (fixture*)user;
    return ++f->clock;
}

static int test_publish(void* user, const rsx_nr_report_record* r, u64 stamp)
{
    fixture* f = (fixture*)user;
    const u32 at = (r->desc.ea - 0x10000000u) / 4u;
    if (at + 3u >= sizeof(f->memory) / sizeof(f->memory[0]))
        return -1;
    f->memory[at + 0u] = (u32)(stamp >> 32);
    f->memory[at + 1u] = (u32)stamp;
    if (r->desc.guest_value_valid)
        f->memory[at + 2u] = r->desc.guest_value;
    f->memory[at + 3u] = 0u;
    f->published_sequence[f->publish_calls++] = r->sequence;
    return 0;
}

static int test_submit(void* user, u64 required, u64* completed)
{
    fixture* f = (fixture*)user;
    f->submit_calls++;
    if (f->completed < required)
        f->completed = required;
    *completed = f->completed;
    return 0;
}

static void fixture_init(fixture* f, int enabled)
{
    memset(f, 0, sizeof(*f));
    rsx_nr_report_scoreboard_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.timestamp = test_timestamp;
    ops.publish = test_publish;
    ops.submit_wait = test_submit;
    rsx_nr_report_scoreboard_init(&f->sb, enabled, &ops);
}

static rsx_nr_report_desc desc(u32 type, u32 slot, u64 generation, u64 fence)
{
    rsx_nr_report_desc d;
    memset(&d, 0, sizeof(d));
    d.type = type;
    d.dma = 0x66626660u;
    d.offset = slot * 16u;
    d.ea = 0x10000000u + d.offset;
    d.query_slot = slot;
    d.guest_value = type == 1u ? 1u : 0u;
    d.guest_value_valid = type >= 1u && type <= 5u;
    d.writer_generation = generation;
    d.recording_fence = fence;
    return d;
}

static int test_many_before_present(void)
{
    static fixture f; fixture_init(&f, 1);
    for (u32 i = 0; i < 1000u; ++i) {
        const rsx_nr_report_desc d = desc(1u + i % 5u, i % 64u, 7u, 9u);
        CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &d) == 0,
              "enqueue %u", i);
    }
    CHECK(f.publish_calls == 0u && f.submit_calls == 0u,
          "premature publication/submit");
    CHECK(rsx_nr_report_scoreboard_complete(&f.sb, 9u, 9u, 1) == 1000,
          "present did not publish complete frame");
    CHECK(f.publish_calls == 1000u && f.submit_calls == 0u &&
              rsx_nr_report_scoreboard_pending(&f.sb) == 0u,
          "present aggregate mismatch");
    for (u32 i = 1; i < 1000u; ++i)
        CHECK(f.published_sequence[i] > f.published_sequence[i - 1u],
              "publication reordered at %u", i);
    return 0;
}

static int test_reader_boundaries(void)
{
    static fixture f; fixture_init(&f, 1);
    rsx_nr_report_desc a = desc(1u, 3u, 1u, 4u);
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &a) == 0, "enqueue a");
    CHECK(rsx_nr_report_scoreboard_consume(
              &f.sb, a.ea + 8u, 4u, RSX_NR_REPORT_READ_PPU) == 0,
          "reader before submission failed");
    CHECK(f.submit_calls == 1u && f.memory[(a.offset + 8u) / 4u] == 1u,
          "early reader did not force exact report");

    rsx_nr_report_desc b = desc(2u, 4u, 1u, 8u);
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &b) == 0, "enqueue b");
    CHECK(rsx_nr_report_scoreboard_complete(&f.sb, 8u, 7u, 1) == 0,
          "incomplete fence published");
    CHECK(rsx_nr_report_scoreboard_pending(&f.sb) == 1u,
          "submitted pending disappeared");
    CHECK(rsx_nr_report_scoreboard_consume(
              &f.sb, b.ea, 16u, RSX_NR_REPORT_READ_SPU) == 0,
          "reader after submit failed");
    CHECK(f.submit_calls == 2u && rsx_nr_report_scoreboard_pending(&f.sb) == 0,
          "reader did not complete submitted report");

    rsx_nr_report_desc c = desc(3u, 5u, 2u, 12u);
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &c) == 0, "enqueue c");
    CHECK(rsx_nr_report_scoreboard_complete(&f.sb, 12u, 12u, 1) == 1,
          "completed fence did not publish");
    const u64 submits = f.submit_calls;
    CHECK(rsx_nr_report_scoreboard_consume(
              &f.sb, c.ea, 16u, RSX_NR_REPORT_READ_HLE) == 0 &&
              f.submit_calls == submits,
          "completed report forced another submit");
    return 0;
}

static int test_overwrite_and_generation(void)
{
    static fixture f; fixture_init(&f, 1);
    rsx_nr_report_desc a = desc(1u, 9u, 4u, 20u);
    rsx_nr_report_desc b = desc(2u, 9u, 4u, 20u);
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &a) == 0 &&
              rsx_nr_report_scoreboard_enqueue(&f.sb, &b) == 0,
          "same-address enqueue failed");
    CHECK(rsx_nr_report_scoreboard_complete(&f.sb, 20u, 20u, 1) == 2,
          "same-address publication count");
    CHECK(f.published_sequence[0] < f.published_sequence[1],
          "same-address writes reordered");
    CHECK(f.memory[(b.offset + 8u) / 4u] == 0u,
          "last overwrite not visible");
    return 0;
}

static int test_reset_shutdown_movie_unknown_disabled(void)
{
    static fixture f; fixture_init(&f, 1);
    rsx_nr_report_desc d = desc(1u, 15u, 1u, 2u);
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &d) == 0, "movie enqueue");
    /* Movie handoff is a natural fenced submission. */
    CHECK(rsx_nr_report_scoreboard_complete(&f.sb, 2u, 2u, 1) == 1,
          "movie handoff publication");
    d.recording_fence = 3u;
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &d) == 0, "reset enqueue");
    rsx_nr_report_scoreboard_reset(&f.sb);
    CHECK(rsx_nr_report_scoreboard_pending(&f.sb) == 0u,
          "reset retained pending report");
    d.recording_fence = 4u;
    CHECK(rsx_nr_report_scoreboard_enqueue(&f.sb, &d) == 0,
          "shutdown enqueue");
    rsx_nr_report_scoreboard_shutdown(&f.sb);
    CHECK(rsx_nr_report_scoreboard_pending(&f.sb) == 0u,
          "shutdown retained pending report");

    static fixture off; fixture_init(&off, 0);
    CHECK(rsx_nr_report_scoreboard_enqueue(&off.sb, &d) ==
              RSX_NR_REPORT_FALLBACK_DISABLED,
          "disabled path did not preserve fallback");
    CHECK(off.submit_calls == 0u && off.publish_calls == 0u,
          "disabled path side effect");
    rsx_nr_report_scoreboard_note_fallback(
        &off.sb, &d, RSX_NR_REPORT_FALLBACK_UNKNOWN_TYPE);
    rsx_nr_report_scoreboard_stats stats;
    rsx_nr_report_scoreboard_get_stats(&off.sb, &stats);
    CHECK(stats.fallback[RSX_NR_REPORT_FALLBACK_DISABLED] == 1u &&
              stats.fallback[RSX_NR_REPORT_FALLBACK_UNKNOWN_TYPE] == 1u,
          "fallback reasons not exact");
    return 0;
}

static int test_sparse_read_watch_default_off_and_cross_page(void)
{
    read_watch_hits = read_watch_ea = read_watch_size =
        read_watch_source = 0u;
    vm_native_report_clear_read_watches();
    vm_native_report_watch_read_range(0x00001FFFu, 2u);
    vm_native_report_notify_read(0x00001FFFu, 2u, 1u);
    CHECK(read_watch_hits == 0u,
          "default-off read watch called observer");

    vm_native_report_set_read_observer(test_read_watch, NULL);
    vm_native_report_notify_read(0x00003000u, 16u, 1u);
    CHECK(read_watch_hits == 0u, "unrelated page called observer");
    vm_native_report_notify_read(0x00002000u, 1u, 1u);
    CHECK(read_watch_hits == 1u && read_watch_ea == 0x00002000u &&
              read_watch_size == 1u && read_watch_source == 1u,
          "cross-page watched byte was missed");

    vm_native_report_clear_read_watches();
    vm_native_report_notify_read(0x00001FFFu, 2u, 1u);
    CHECK(read_watch_hits == 1u, "cleared page remained watched");
    vm_native_report_watch_read_range(0x00001FFFu, 2u);
    vm_native_report_set_read_observer(NULL, NULL);
    vm_native_report_notify_read(0x00001FFFu, 2u, 1u);
    CHECK(read_watch_hits == 1u, "disabled observer remained active");
    vm_native_report_clear_read_watches();
    return 0;
}

int main(void)
{
    if (test_many_before_present() || test_reader_boundaries() ||
        test_overwrite_and_generation() ||
        test_reset_shutdown_movie_unknown_disabled() ||
        test_sparse_read_watch_default_off_and_cross_page())
        return 1;
    puts("rsx_nr_report_scoreboard: PASS");
    return 0;
}
