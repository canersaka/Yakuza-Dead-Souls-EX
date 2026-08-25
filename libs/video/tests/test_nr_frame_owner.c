#include "rsx_nr_frame_owner.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c, ...) do { if (!(c)) { \
    fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); return 1; } } while (0)

#define TEST_WORDS (0x8000u / 4u)
#define TEST_RING_OPS 512u
#define TEST_RING_SIDE 32768u

typedef struct fixture {
    u32 words[TEST_WORDS];
    u32 semaphore;
    u32 references;
    u32 presents;
    u32 published_put;
    u32 stopper_release_calls;
    u32 island_get;
    u32 island_put;
    u32 island_resume;
    u32 island_late_entry;
    u32 released_source;
    u32 released_command;
    u32 released_target_word;
    u32 repair_source;
    u32 repair_put;
    u32 repair_command;
    u32 repair_target;
    u32 repair_word;
    u32 repair_resume;
    u32 repair_pending;
    u32 repair_calls;
    u32 hole_get;
    u32 hole_put;
    u32 hole_word;
    u32 hole_resume;
    u32 hole_pending;
    u32 hole_calls;
    u32 hole_previous_get;
    u32 hole_previous_command;
    unsigned long long now_ms;
    rsx_nr_slot slots[TEST_RING_OPS];
    u32 side[TEST_RING_SIDE];
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nir_adapter adapter;
    rsx_nr_backend backend;
    rsx_nr_frame_owner owner;
} fixture;

static int read_word(void* user, u32 io, u32* value)
{
    fixture* f = user;
    if (!value || (io & 3u) || io >= sizeof(f->words))
        return -1;
    *value = f->words[io >> 2];
    return 0;
}

static unsigned long long test_now_ms(void* user)
{
    return ((fixture*)user)->now_ms;
}

static int sem_read(void* user, u32 dma, u32 offset, u32* value)
{
    fixture* f = user;
    if (!value || dma != 0x66626660u || offset != 0x100u)
        return -1;
    *value = f->semaphore;
    return 0;
}

static void set_reference(void* user, u32 value)
{
    fixture* f = user;
    f->references = value;
}

static int present(void* user, u32 buffer)
{
    fixture* f = user;
    f->presents++;
    return buffer < 8u ? 0 : -1;
}

static int release_initial_stopper(void* user, u32 get, u32 put,
                                   u32 command, u32* resume_get)
{
    fixture* f = user;
    f->stopper_release_calls++;
    if (!resume_get || get != 0x1000u || f->published_put != put ||
        put <= get + 4u || f->words[get >> 2] != command)
        return 0;
    *resume_get = get + 4u;
    f->words[get >> 2] = 0x20000000u | *resume_get;
    return 1;
}

static int registered_island_edge(void* user, u32 get, u32 put,
                                  u32 command, u32* resume_get)
{
    fixture* f = user;
    (void)command;
    if (!resume_get || !f->island_get || get != f->island_get ||
        put != f->island_put || !f->island_resume)
        return 0;
    *resume_get = f->island_resume;
    return f->island_late_entry ? 2 : 1;
}

static int registered_released_edge(void* user, u32 get, u32 put,
                                    u32 command, u32 target,
                                    u32 target_word)
{
    fixture* f = user;
    (void)put;
    return get == f->released_source &&
           command == f->released_command &&
           target == get + 4u && target_word == f->released_target_word;
}

static int resolve_generated_jump(void* user, u32 get, u32 put,
                                  u32 command, u32 target,
                                  u32 target_word, u32* resume_get)
{
    fixture* f = user;
    f->repair_calls++;
    if (!resume_get || get != f->repair_source || put != f->repair_put ||
        command != f->repair_command || target != f->repair_target ||
        target_word != f->repair_word)
        return 0;
    if (f->repair_pending) {
        f->repair_pending--;
        return -1;
    }
    if (!f->repair_resume)
        return 0;
    f->words[get >> 2] = 0x20000000u | f->repair_resume;
    *resume_get = f->repair_resume;
    return 1;
}

static int resolve_generated_hole(void* user, u32 get, u32 put,
                                  u32 word, u32 previous_get,
                                  u32 previous_command, u32* resume_get)
{
    fixture* f = user;
    f->hole_previous_get = previous_get;
    f->hole_previous_command = previous_command;
    f->hole_calls++;
    if (!resume_get || get != f->hole_get || put != f->hole_put ||
        word != f->hole_word)
        return 0;
    if (f->hole_pending) {
        f->hole_pending--;
        return -1;
    }
    if (!f->hole_resume)
        return 0;
    *resume_get = f->hole_resume;
    return 1;
}

static void fixture_init(fixture* f)
{
    memset(f, 0, sizeof(*f));
    rsx_nr_ring_init_fixed(&f->ring, f->slots, TEST_RING_OPS,
                           f->side, TEST_RING_SIDE);
    rsx_nr_tokens_init(&f->tokens);
    const rsx_nir_sink sink = rsx_nr_ring_sink(&f->ring);
    rsx_nir_adapter_init_sink(&f->adapter, &sink);
    f->adapter.shadow_mode = 0;
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = f;
    ops.sem_read = sem_read;
    ops.set_reference = set_reference;
    ops.present = present;
    rsx_nr_backend_init(&f->backend, &f->ring, &f->tokens, &ops);
    rsx_nr_frame_owner_init(&f->owner, &f->adapter, &f->backend,
                             &f->ring, read_word, f,
                             release_initial_stopper, f,
                             registered_island_edge, f,
                             registered_released_edge, f,
                             resolve_generated_jump, f,
                             resolve_generated_hole, f);
}

static u32 packet(u32 count, u32 method)
{
    return (count << 18) | method;
}

static int test_consume_once_and_present(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    f.words[(base + 0u) >> 2] = packet(1u, 0x0050u);
    f.words[(base + 4u) >> 2] = 0x12345678u;
    f.words[(base + 8u) >> 2] = packet(1u, 0xE944u);
    f.words[(base + 12u) >> 2] = 2u;
    u32 next = 0, ret = 0;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 16u, ~0u, &next, &ret) ==
          RSX_NR_FRAME_ADVANCED && next == base + 8u,
          "reference packet did not advance");
    CHECK(f.references == 0x12345678u && f.adapter.methods_seen == 1u,
          "reference was not translated/executed once");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, next, base + 16u, ret, &next, &ret) ==
          RSX_NR_FRAME_ADVANCED && next == base + 16u,
          "present packet did not advance");
    CHECK(f.presents == 1u && f.adapter.methods_seen == 2u &&
              f.owner.stats.frames == 1u,
          "present counts wrong (%u/%u/%llu)", f.presents,
          f.adapter.methods_seen, f.owner.stats.frames);
    return 0;
}

static int test_semaphore_retry_is_not_retranslated(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    f.words[(base + 0u) >> 2] = packet(3u, 0x0060u);
    f.words[(base + 4u) >> 2] = 0x66626660u;
    f.words[(base + 8u) >> 2] = 0x100u;
    f.words[(base + 12u) >> 2] = 7u;
    u32 next = 0, ret = 0;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 16u, ~0u, &next, &ret) ==
          RSX_NR_FRAME_WAIT_SEMAPHORE,
          "unsatisfied acquire did not block");
    CHECK(f.adapter.methods_seen == 3u && f.owner.stats.methods == 3u,
          "first acquire translated wrong count");
    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, base, base + 16u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_WAIT_SEMAPHORE,
              "retry %u did not remain blocked", i);
    CHECK(f.adapter.methods_seen == 3u && f.owner.stats.methods == 3u &&
              f.owner.packet_index == 2u && f.owner.method_inflight,
          "blocked retries retranslated the method");
    f.semaphore = 7u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 16u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == base + 16u,
          "satisfied acquire did not retire packet");
    CHECK(f.adapter.methods_seen == 3u &&
              f.backend.stats.executed[RSX_NIR_OP_SEMAPHORE_ACQUIRE] == 1u,
          "acquire was duplicated");
    return 0;
}

static int test_partial_stopper_and_flow(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    u32 next = 0, ret = 0;
    f.words[base >> 2] = packet(1u, 0x0050u);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 4u, ~0u, &next, &ret) ==
          RSX_NR_FRAME_WAIT_PARTIAL,
          "partial packet was consumed");
    f.words[base >> 2] = 0x20000000u | base;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 4u, ~0u, &next, &ret) ==
          RSX_NR_FRAME_WAIT_STOPPER,
          "self stopper did not wait");
    f.words[base >> 2] = 0x20000000u | (base + 8u);
    f.words[(base + 8u) >> 2] = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 12u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == base + 8u,
          "jump did not advance to target");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, next, base + 12u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == base + 12u,
          "target NOP did not advance");
    return 0;
}

static int test_exact_published_head_stopper_release(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 put = base + 12u;
    u32 next = 0, ret = ~0u;
    f.words[base >> 2] = 0x20000000u | base;
    f.words[(base + 4u) >> 2] = 0u;
    f.published_put = put;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == base + 4u,
          "stable published head stopper did not advance exactly once");
    CHECK(f.stopper_release_calls == 1u &&
              f.words[base >> 2] == (0x20000000u | (base + 4u)) &&
              f.owner.stats.released_stoppers == 1u,
          "published head release did not retain exact accounting");

    fixture_init(&f);
    f.words[base >> 2] = 0x20000000u | base;
    f.words[(base + 4u) >> 2] = 0u;
    f.published_put = put + 4u; /* publication changed before recheck */
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, put, ~0u, &next, &ret) ==
              RSX_NR_FRAME_WAIT_STOPPER && next == base,
          "unstable publication snapshot skipped the stopper");
    CHECK(f.words[base >> 2] == (0x20000000u | base) &&
              f.owner.stats.released_stoppers == 0u,
          "unstable publication mutated the stopper");

    fixture_init(&f);
    const u32 ordinary = 0x2000u;
    f.words[ordinary >> 2] = 0x20000000u | ordinary;
    f.published_put = ordinary + 12u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, ordinary, f.published_put, ~0u,
              &next, &ret) == RSX_NR_FRAME_WAIT_STOPPER,
          "ordinary in-stream stopper was released as a startup guard");
    CHECK(next == ordinary &&
              f.words[ordinary >> 2] == (0x20000000u | ordinary),
          "ordinary stopper was changed or skipped");
    return 0;
}

static int test_call_return_and_exact_unmapped_failure(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 target = 0x1800u;
    u32 next = 0, ret = 0;
    f.words[base >> 2] = target | 2u;
    f.words[target >> 2] = 0x00020000u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 4u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == target &&
              ret == base + 4u,
          "CALL did not enter the separately published target");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, next, base + 4u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == base + 4u && ret == ~0u,
          "RETURN did not restore the primary FIFO cursor");

    fixture_init(&f);
    const u32 unmapped = 0x10000u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, unmapped, unmapped + 4u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_FATAL,
          "unmapped strict cursor did not fail");
    CHECK(f.owner.failure.kind == RSX_NR_FRAME_FAILURE_UNMAPPED &&
              f.owner.failure.get == unmapped,
          "unmapped strict cursor was not recorded exactly");
    return 0;
}

static int test_called_list_entry_waits_for_final_publication(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 target = 0x1800u;
    const u32 transient_nested_call = 0x8E021702u;
    u32 next = 0, ret = ~0u;
    f.words[base >> 2] = target | 2u;
    f.words[target >> 2] = transient_nested_call;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 4u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == target &&
              ret == base + 4u,
          "CALL did not enter the target publication boundary");
    CHECK(f.owner.flow_origin.get == base &&
              f.owner.flow_origin.command == (target | 2u) &&
              f.owner.flow_origin.target == target &&
              f.owner.flow_origin.return_before == ~0u &&
              f.owner.flow_origin.return_after == base + 4u,
          "CALL origin was not retained exactly");
    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, target, base + 4u, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == target &&
                  ret == base + 4u,
              "transient called-list word escaped at retry %u", i);
    CHECK(!f.owner.fatal && f.owner.flow_wait_polls == 1000000u &&
              f.owner.stats.control_words == 1u,
          "transient called-list word did not remain one wait episode");
    f.words[target >> 2] = 0x00020000u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, target, base + 4u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == base + 4u && ret == ~0u,
          "finalized called list did not return exactly once");
    return 0;
}

static int test_jump_waits_for_exact_target_publication(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 target = 0x2000u;
    const u32 stale = 0x43260000u;
    u32 next = 0, ret = ~0u;
    f.words[base >> 2] = 0x20000000u | target;
    f.words[target >> 2] = stale;
    for (u32 i = 0; i < 1000000u; ++i) {
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, base, base + 8u, ~0u, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == base && ret == ~0u,
              "unfinalized jump target escaped at retry %u", i);
    }
    CHECK(!f.owner.fatal && f.owner.flow_wait_polls == 1000000u &&
              f.owner.stats.control_words == 0u,
          "unfinalized jump was executed, skipped, or failed early");

    f.words[target >> 2] = packet(1u, 0x0050u);
    f.words[(target + 4u) >> 2] = 0x11223344u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 8u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == target,
          "published jump target did not advance from the source");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, target, base + 8u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == target + 8u &&
              f.references == 0x11223344u,
          "published target packet did not execute exactly once");

    fixture_init(&f);
    f.words[base >> 2] = 0x20000000u | target;
    f.words[target >> 2] = stale;
    f.owner.flow_wait_limit = 3u;
    for (u32 i = 0; i < 3u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, base, base + 8u, ~0u, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL,
              "bounded target wait failed before limit at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 8u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_FATAL &&
              f.owner.failure.kind == RSX_NR_FRAME_FAILURE_BAD_FLOW &&
              f.owner.failure.get == base &&
              f.owner.failure.put == base + 8u &&
              f.owner.failure.command == (0x20000000u | target) &&
              f.owner.failure.method == target &&
              f.owner.failure.argument == stale &&
              f.owner.failure.argument_index == 4u,
          "bounded target wait did not retain the exact dependency");

    /* A recycled segment tail can contain data which happens to decode as a
     * supported packet. The producer reserves that exact word for its link,
     * so no packet-shaped value may release the dependency. */
    fixture_init(&f);
    const u32 segment_tail = 0x1FFCu;
    const u32 segment_next = 0x2000u;
    f.owner.primary_segment_bytes = 0x2000u;
    f.words[base >> 2] = 0x20000000u | segment_tail;
    f.words[segment_tail >> 2] = packet(1u, 0x0050u);
    f.words[(segment_tail + 4u) >> 2] = 0xAABBCCDDu;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, 0x3000u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && next == base,
          "packet-shaped segment-tail data was consumed as a command");
    f.words[segment_tail >> 2] =
        0x20000000u | segment_next;
    f.words[segment_next >> 2] = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, 0x3000u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == segment_tail,
          "published segment-tail link did not release its incoming jump");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, next, 0x3000u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == segment_next,
          "published segment-tail link did not execute");

    /* EDGE generated lists use a smaller independent 128 KiB block tail.
     * Model it at 8 KiB so the fixed fixture can prove that a packet-shaped
     * recycled word is never admitted as the target of an incoming jump. */
    fixture_init(&f);
    f.owner.primary_segment_bytes = 0x4000u;
    f.owner.generated_block_bytes = 0x2000u;
    f.words[base >> 2] = 0x20000000u | segment_tail;
    f.words[segment_tail >> 2] = packet(1u, 0x0050u);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, 0x3000u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && next == base,
          "packet-shaped generated-block tail was consumed");
    f.words[segment_tail >> 2] = 0x20000000u | segment_next;
    f.words[segment_next >> 2] = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, 0x3000u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == segment_tail,
          "generated-block tail link did not release its incoming jump");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, next, 0x3000u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == segment_next,
          "generated-block tail link did not execute");

    /* The other live shape is a fenced forward stopper release from the
     * preceding word into a real packet whose header itself occupies the
     * generated-block tail.  Byte shape alone remains insufficient: it must
     * park without the exact producer record, then execute normally once the
     * matching record is visible. */
    fixture_init(&f);
    f.owner.primary_segment_bytes = 0x4000u;
    f.owner.generated_block_bytes = 0x2000u;
    const u32 release_source = segment_tail - 4u;
    const u32 release_command = 0x20000000u | segment_tail;
    f.words[release_source >> 2] = release_command;
    f.words[segment_tail >> 2] = packet(1u, 0x0050u);
    f.words[segment_next >> 2] = 0x55AA33CCu;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, release_source, 0x3000u, ~0u,
              &next, &ret) == RSX_NR_FRAME_WAIT_PARTIAL &&
              next == release_source,
          "unproven packet-shaped boundary did not remain parked");
    f.released_source = release_source;
    f.released_command = release_command;
    f.released_target_word = f.words[segment_tail >> 2];
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, release_source, 0x3000u, ~0u,
              &next, &ret) == RSX_NR_FRAME_ADVANCED &&
              next == segment_tail &&
              f.owner.stats.admitted_released_boundaries == 1u,
          "exact fenced boundary release was not admitted once");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, next, 0x3000u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == segment_next + 4u &&
              f.references == 0x55AA33CCu,
          "released cross-boundary packet was not executed exactly once");

    return 0;
}

static int test_invalid_jump_repair_is_exact_and_latched(void)
{
    fixture f;
    fixture_init(&f);
    const u32 source = 0x1000u;
    const u32 raw_target = 0x2000u;
    const u32 resume = 0x2100u;
    const u32 put = 0x3000u;
    const u32 raw = 0x43260000u;
    u32 next = source, ret = ~0u;
    f.words[source >> 2] = 0x20000000u | raw_target;
    f.words[raw_target >> 2] = raw;
    f.repair_source = source;
    f.repair_put = put;
    f.repair_command = f.words[source >> 2];
    f.repair_target = raw_target;
    f.repair_word = raw;

    /* A refused proof is attempted once for an unchanged dependency, even
     * across a million consumer polls. */
    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, source, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == source,
              "refused generated-link proof escaped at retry %u", i);
    CHECK(f.repair_calls == 1u &&
              f.owner.stats.generated_link_attempts == 1u &&
              f.owner.stats.repaired_generated_links == 0u,
          "unchanged invalid target was rescanned (%u/%llu)",
          f.repair_calls, f.owner.stats.generated_link_attempts);

    /* A genuinely newer PUT generation permits one new proof only after that
     * snapshot is stable for the same bounded interval. The callback must
     * patch the source, and the owner revalidates both the patched jump and
     * resume command before advancing. Raw target bytes are never dispatched. */
    const u32 new_put = put + 0x100u;
    f.repair_put = new_put;
    f.repair_resume = resume;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0xAABBCCDDu;
    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, source, new_put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == source,
              "new PUT generation was scanned before stable at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, source, new_put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume,
          "proven generated-link repair did not advance exactly");
    CHECK(f.repair_calls == 2u &&
              f.owner.stats.generated_link_attempts == 2u &&
              f.owner.stats.repaired_generated_links == 1u &&
              f.owner.stats.methods == 0u &&
              f.words[source >> 2] == (0x20000000u | resume),
          "generated-link repair accounting or source patch was wrong");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, resume, new_put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && f.references == 0xAABBCCDDu,
          "repaired command stream did not execute exactly once");
    return 0;
}

static int test_publication_timeout_uses_wall_time_not_poll_rate(void)
{
    fixture f;
    fixture_init(&f);
    const u32 source = 0x1000u;
    const u32 target = 0x2000u;
    u32 put = 0x3000u;
    u32 next = source, ret = ~0u;
    f.words[source >> 2] = 0x20000000u | target;
    f.words[target >> 2] = 0x43260000u;
    f.repair_source = source;
    f.repair_command = f.words[source >> 2];
    f.repair_target = target;
    f.repair_word = f.words[target >> 2];
    f.now_ms = 1000u;
    rsx_nr_frame_owner_set_publication_clock(
        &f.owner, test_now_ms, &f, 2u, 100u);

    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, source, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && !f.owner.fatal,
              "wall-time publication wait failed from poll rate at %u", i);
    CHECK(f.repair_calls == 0u,
          "proof ran before the wall-time delay (%u)", f.repair_calls);

    f.now_ms = 1003u;
    f.owner.flow_wait_clock_next_poll = 0u;
    f.repair_put = put;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, source, put, ret, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && f.repair_calls == 1u,
          "wall-time proof did not run once after its delay");

    /* Unrelated producer progress changes PUT but not this dependency. It may
     * permit a new bounded proof, but it must not restart the failure clock. */
    put += 0x100u;
    f.repair_put = put;
    f.now_ms = 1099u;
    f.owner.flow_wait_clock_next_poll = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, source, put, ret, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && !f.owner.fatal,
          "publication wait failed before wall-time bound");
    put += 0x100u;
    f.repair_put = put;
    f.now_ms = 1100u;
    f.owner.flow_wait_clock_next_poll = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, source, put, ret, &next, &ret) ==
              RSX_NR_FRAME_FATAL && f.owner.fatal &&
              f.owner.failure.kind == RSX_NR_FRAME_FAILURE_BAD_FLOW,
          "changing PUT incorrectly reset the wall-time failure bound");
    return 0;
}

static int test_late_published_island_entry_preserves_payload(void)
{
    fixture f;
    fixture_init(&f);
    const u32 payload = 0x1050u;
    const u32 false_target = 0x2060u;
    const u32 resume = 0x2200u;
    const u32 put = 0x3000u;
    const u32 vp_word = 0x20000000u | false_target;
    u32 next = payload, ret = ~0u;
    f.words[payload >> 2] = vp_word;
    /* Raw payload can target bytes which themselves look executable. The
     * producer record must win before normal flow-target admission. */
    f.words[false_target >> 2] = packet(1u, 0x0050u);
    f.words[(false_target + 4u) >> 2] = 0xDEADBEEFu;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0x12345678u;
    f.island_get = payload;
    f.island_put = put;
    f.island_resume = resume;
    f.island_late_entry = 1u;

    CHECK(rsx_nr_frame_owner_step(
              &f.owner, payload, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume,
          "recorded late island entry did not advance to its exact end");
    CHECK(f.words[payload >> 2] == vp_word &&
              f.owner.stats.skipped_data_islands == 1u &&
              f.owner.stats.recovered_late_island_entries == 1u &&
              f.owner.stats.repaired_generated_links == 0u &&
              f.repair_calls == 0u &&
              f.owner.stats.methods == 0u,
          "late island recovery rewrote/dispatched payload or miscounted");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, resume, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && f.references == 0x12345678u,
          "late island resume command did not execute exactly once");
    return 0;
}

static int test_primary_generated_hole_proof_is_exact_and_latched(void)
{
    fixture f;
    fixture_init(&f);
    const u32 hole = 0x1278u;
    const u32 resume = 0x1280u;
    const u32 put = 0x3000u;
    const u32 raw = 0x3A2AAAABu;
    u32 next = hole, ret = ~0u;
    f.words[hole >> 2] = raw;
    f.words[(hole + 4u) >> 2] = 0x04000000u;
    f.hole_get = hole;
    f.hole_put = put;
    f.hole_word = raw;
    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "refused generated hole escaped at retry %u", i);
    CHECK(f.hole_calls == 1u &&
              f.owner.stats.repaired_generated_holes == 0u,
          "unchanged generated hole was rescanned (%u)", f.hole_calls);

    const u32 new_put = put + 0x100u;
    f.hole_put = new_put;
    f.hole_resume = resume;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0x12345678u;
    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, new_put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "new hole PUT generation was scanned before stable at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, new_put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume,
          "proven generated hole did not advance to its exact prologue");
    CHECK(f.hole_calls == 2u &&
              f.owner.stats.repaired_generated_holes == 1u &&
              f.owner.stats.methods == 0u,
          "generated hole proof dispatched raw bytes or counted wrongly");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, resume, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && f.references == 0x12345678u,
          "generated hole resume did not execute exactly once");
    return 0;
}

static int test_pending_generated_hole_rechecks_without_put_change(void)
{
    fixture f;
    fixture_init(&f);
    const u32 hole = 0x1278u;
    const u32 resume = 0x1280u;
    const u32 put = 0x3000u;
    const u32 raw = 0x3A2AAAABu;
    u32 next = hole, ret = ~0u;
    f.words[hole >> 2] = raw;
    f.hole_get = hole;
    f.hole_put = put;
    f.hole_word = raw;
    f.hole_pending = 1u;

    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "pending hole was tested before proof delay at %u", i);
    CHECK(f.hole_calls == 0u,
          "pending hole callback ran before its proof delay");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, put, ret, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && f.hole_calls == 1u,
          "pending producer result was not retained as a wait");

    /* The generated target becomes ready behind the already-published PUT.
     * A second proof is allowed only after another complete delay; the raw
     * words are never dispatched during either interval. */
    f.hole_resume = resume;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0x12345678u;
    for (u32 i = 0; i < (1u << 16) - 1u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "pending hole rescanned too early at %u", i);
    CHECK(f.hole_calls == 1u && f.adapter.methods_seen == 0u,
          "pending hole was rescanned or dispatched during delay");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume &&
              f.hole_calls == 2u &&
              f.owner.stats.repaired_generated_holes == 1u,
          "ready generated bytes were not reproven under unchanged PUT");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, resume, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && f.references == 0x12345678u,
          "reproven generated resume did not execute once");
    return 0;
}

static int test_generated_hole_postproof_reread_retries(void)
{
    fixture f;
    fixture_init(&f);
    const u32 hole = 0x1278u;
    const u32 resume = 0x1280u;
    const u32 put = 0x3000u;
    const u32 raw = 0x3A2AAAABu;
    u32 next = hole, ret = ~0u;
    f.words[hole >> 2] = raw;
    f.hole_get = hole;
    f.hole_put = put;
    f.hole_word = raw;
    f.hole_resume = resume;
    f.words[resume >> 2] = 0x3A2AAAABu;

    /* The callback proves the producer identity, but the independent owner
     * reread still sees an unready resume word.  This is a pending
     * publication race, not a permanent refusal for this unchanged PUT. */
    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "post-proof hole was tested before delay at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, put, ret, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && f.hole_calls == 1u,
          "failed post-proof reread did not remain pending");

    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0x89ABCDEFu;
    for (u32 i = 0; i < (1u << 16) - 1u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "post-proof hole retried too early at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume &&
              f.hole_calls == 2u &&
              f.owner.stats.repaired_generated_holes == 1u,
          "ready post-proof hole was not retried under unchanged PUT");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, resume, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && f.references == 0x89ABCDEFu,
          "post-proof retry did not execute the resume exactly once");
    return 0;
}

static int test_generated_hole_put_progress_resets_failure_bound(void)
{
    fixture f;
    fixture_init(&f);
    const u32 hole = 0x1278u;
    const u32 raw = 0x3A2AAAABu;
    u32 next = hole, ret = ~0u;
    f.words[hole >> 2] = raw;
    f.hole_get = hole;
    f.hole_word = raw;
    f.owner.flow_wait_limit = 3u;

    /* Every PUT value is a new publication generation.  More total polls
     * than the unchanged-generation bound must remain a wait while the
     * producer is demonstrably advancing. */
    for (u32 i = 0; i < 64u; ++i) {
        const u32 put = 0x2000u + i * 4u;
        f.hole_put = put;
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole && !f.owner.fatal,
              "advancing PUT consumed the failure bound at generation %u", i);
    }
    CHECK(f.owner.flow_wait_polls == 1u,
          "new PUT generation retained %u stale failure polls",
          f.owner.flow_wait_polls);

    /* Once publication stops changing, the ordinary bounded failure remains
     * intact; this is not an unbounded malformed-command escape. */
    const u32 stable_put = f.hole_put;
    for (u32 i = 0; i < 2u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, stable_put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL,
              "stable PUT failed before its unchanged bound at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, stable_put, ret, &next, &ret) ==
              RSX_NR_FRAME_FATAL && f.owner.fatal,
          "stable malformed generation did not retain the bounded failure");
    return 0;
}

static int test_generated_hole_receives_owner_predecessor(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1200u;
    const u32 command = packet(1u, 0x0050u);
    const u32 hole = base + 8u;
    const u32 resume = hole + 8u;
    const u32 put = 0x3000u;
    u32 next = base, ret = ~0u;
    f.words[base >> 2] = command;
    f.words[(base + 4u) >> 2] = 0x01020304u;
    f.words[hole >> 2] = 0x3A2AAAABu;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0x05060708u;
    f.hole_get = hole;
    f.hole_put = put;
    f.hole_word = f.words[hole >> 2];
    f.hole_resume = resume;

    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == hole,
          "predecessor packet did not advance to generated gap");
    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL,
              "predecessor-proven gap escaped before delay at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume &&
              f.hole_previous_get == base &&
              f.hole_previous_command == command,
          "hole resolver received predecessor %08X/%08X expected %08X/%08X",
          f.hole_previous_get, f.hole_previous_command, base, command);
    return 0;
}

static int test_primary_unmapped_flow_waits_for_in_place_publication(void)
{
    fixture f;
    fixture_init(&f);
    const u32 get = 0x1158u;
    const u32 put = 0x3000u;
    const u32 raw_jump = 0xBF820821u;
    u32 next = get, ret = ~0u;
    f.words[get >> 2] = raw_jump;
    f.owner.flow_wait_limit = 3u;

    CHECK(rsx_nr_frame_owner_step(
              &f.owner, get, put, ret, &next, &ret) ==
              RSX_NR_FRAME_WAIT_PARTIAL && next == get && !f.owner.fatal,
          "unmapped flow-shaped publication word failed immediately");
    f.words[get >> 2] = packet(1u, 0x0050u);
    f.words[(get + 4u) >> 2] = 0x13572468u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, get, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == get + 8u &&
              f.references == 0x13572468u && !f.owner.fatal,
          "in-place published method did not execute exactly once");

    /* Reuse the fixed fixture rather than placing two full command arenas on
     * the Windows test stack at once. */
    fixture_init(&f);
    f.words[get >> 2] = raw_jump;
    f.owner.flow_wait_limit = 3u;
    next = get;
    ret = ~0u;
    for (u32 i = 0; i < 3u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, get, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL,
              "stable unmapped flow failed before bound at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, get, put, ret, &next, &ret) ==
              RSX_NR_FRAME_FATAL && f.owner.fatal,
          "stable unmapped flow escaped bounded failure");
    return 0;
}

static int test_packet_shaped_generated_hole_executes_no_arguments(void)
{
    fixture f;
    fixture_init(&f);
    const u32 hole = 0x1230u;
    const u32 resume = 0x1280u;
    const u32 put = 0x3000u;
    const u32 packet_shaped_raw = 0x43C04000u;
    u32 next = hole, ret = ~0u;
    f.words[hole >> 2] = packet_shaped_raw;
    f.words[(hole + 4u) >> 2] = 0u;
    f.hole_get = hole;
    f.hole_put = put;
    f.hole_word = packet_shaped_raw;
    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "packet-shaped raw data escaped at retry %u", i);
    CHECK(f.hole_calls == 1u && f.adapter.methods_seen == 0u &&
              f.owner.stats.methods == 0u && !f.owner.packet_active,
          "packet-shaped raw data was scanned/executed calls=%u seen=%u "
          "methods=%llu active=%u polls=%u",
          f.hole_calls, f.adapter.methods_seen, f.owner.stats.methods,
          f.owner.packet_active, f.owner.flow_wait_polls);

    f.hole_word = packet_shaped_raw + 4u;
    f.words[hole >> 2] = f.hole_word;
    f.hole_resume = resume;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0xCAFEBABEu;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume &&
              f.adapter.methods_seen == 0u,
          "proven packet-shaped data gap did not skip to the exact prologue");
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, resume, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && f.references == 0xCAFEBABEu,
          "packet-shaped gap resume did not execute once");
    return 0;
}

static int test_noop_preserves_sequential_gap_provenance(void)
{
    fixture f;
    fixture_init(&f);
    const u32 noop = 0x1254u;
    const u32 gap = 0x1258u;
    const u32 resume = 0x1280u;
    const u32 put = 0x3000u;
    u32 next = noop, ret = ~0u;
    f.words[noop >> 2] = 0u;
    f.words[gap >> 2] = 0x44C00000u;
    f.hole_get = gap;
    f.hole_put = put;
    f.hole_word = f.words[gap >> 2];
    f.hole_resume = resume;
    f.words[resume >> 2] = packet(1u, 0x0050u);
    f.words[(resume + 4u) >> 2] = 0x12345678u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, noop, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == gap,
          "NOOP before generated gap did not advance exactly");
    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, gap, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL,
              "generated gap escaped before proof delay at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, gap, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume &&
              f.hole_previous_get == noop &&
              f.hole_previous_command == 0u &&
              f.owner.stats.methods == 0u,
          "NOOP sequential provenance was not delivered to exact gap proof");

    /* The warm-repeat payload begins with 0x3F000000, which aliases an old
     * JUMP to an address outside the FIFO. It must enter the same delayed,
     * exact hole proof rather than either following or permanently parking on
     * that raw word. */
    fixture_init(&f);
    const u32 variable_noop = 0x1244u;
    const u32 variable_gap = 0x1248u;
    const u32 variable_resume = 0x1280u;
    next = variable_noop;
    ret = ~0u;
    f.words[variable_noop >> 2] = 0u;
    f.words[variable_gap >> 2] = 0x3F000000u;
    f.hole_get = variable_gap;
    f.hole_put = put;
    f.hole_word = f.words[variable_gap >> 2];
    f.hole_resume = variable_resume;
    f.words[variable_resume >> 2] = packet(1u, 0x0050u);
    f.words[(variable_resume + 4u) >> 2] = 0x89ABCDEFu;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, variable_noop, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == variable_gap,
          "NOOP before flow-shaped generated gap did not advance");
    for (u32 i = 0; i < (1u << 16); ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, variable_gap, put, ret, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL,
              "flow-shaped generated gap escaped before proof at %u", i);
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, variable_gap, put, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == variable_resume &&
              f.hole_previous_get == variable_noop &&
              f.hole_previous_command == 0u && f.hole_calls == 1u &&
              f.owner.stats.methods == 0u,
          "unmapped flow-shaped payload did not use exact NOOP gap proof");
    return 0;
}

static int test_primary_hole_waits_but_called_list_fails(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 hole = base + 8u;
    const u32 stale = 0x3A2AAAABu;
    u32 next = 0, ret = ~0u;
    f.words[base >> 2] = packet(1u, 0x0050u);
    f.words[(base + 4u) >> 2] = 1u;
    f.words[hole >> 2] = stale;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 16u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == hole,
          "packet before the primary hole did not retire");
    for (u32 i = 0; i < 1000000u; ++i)
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, hole, base + 16u, ~0u, &next, &ret) ==
                  RSX_NR_FRAME_WAIT_PARTIAL && next == hole,
              "primary unfinalized hole escaped at retry %u", i);
    CHECK(!f.owner.fatal && f.owner.flow_wait_polls == 1000000u,
          "primary unfinalized hole did not remain one bounded episode");
    f.words[hole >> 2] = packet(1u, 0x0050u);
    f.words[(hole + 4u) >> 2] = 2u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, base + 16u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == hole + 8u &&
              f.references == 2u,
          "published primary hole did not execute exactly once");

    fixture_init(&f);
    f.words[hole >> 2] = stale;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, hole, base + 16u, base + 4u, &next, &ret) ==
              RSX_NR_FRAME_FATAL &&
              f.owner.failure.kind == RSX_NR_FRAME_FAILURE_BAD_FLOW &&
              f.owner.failure.get == hole &&
              f.owner.failure.command == stale,
          "complete called-list corruption was mistaken for publication");
    return 0;
}

static int test_registered_island_edge_skips_payload_exactly(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 payload = 0x1800u;
    const u32 resume = 0x2000u;
    const u32 put = 0x3000u;
    u32 next = 0, ret = ~0u;
    f.island_get = base;
    f.island_put = put;
    f.island_resume = resume;
    f.words[base >> 2] = 0x20000000u | payload;
    f.words[payload >> 2] = 0x2041FFFCu; /* valid VP word, ambiguous JUMP */
    f.words[resume >> 2] = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, put, ~0u, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED && next == resume &&
              f.owner.stats.skipped_data_islands == 1u &&
              f.owner.stats.methods == 0u,
          "registered island payload was decoded instead of skipped");

    fixture_init(&f);
    f.island_get = base;
    f.island_put = put;
    f.island_resume = 0x9000u; /* callback supplied an unmapped bad edge */
    f.words[base >> 2] = 0x20000000u | payload;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, put, ~0u, &next, &ret) ==
              RSX_NR_FRAME_FATAL &&
              f.owner.failure.kind == RSX_NR_FRAME_FAILURE_ISLAND_EDGE &&
              f.owner.failure.get == base &&
              f.owner.failure.method == 0x9000u &&
              f.owner.failure.argument == payload,
          "invalid producer island edge was not bounded exactly");
    return 0;
}

static int test_control_cycle_is_bounded(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    const u32 target = 0x1800u;
    f.words[base >> 2] = 0x20000000u | target;
    f.words[target >> 2] = 0x20000000u | base;
    u32 next = base, ret = ~0u;
    for (u32 i = 0; i < 4096u; ++i) {
        const u32 current = next;
        CHECK(rsx_nr_frame_owner_step(
                  &f.owner, current, base + 4u, ret, &next, &ret) ==
              RSX_NR_FRAME_ADVANCED,
              "control cycle stopped before the fixed bound at %u", i);
    }
    const u32 failed_get = next;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, failed_get, base + 4u, ret, &next, &ret) ==
          RSX_NR_FRAME_FATAL,
          "control-only cycle was not stopped at the fixed bound");
    CHECK(f.owner.failure.kind == RSX_NR_FRAME_FAILURE_BAD_FLOW &&
              f.owner.failure.get == failed_get &&
              f.owner.failure.argument_index == 4097u,
          "control-cycle failure was not retained exactly");
    return 0;
}

static int test_first_unsupported_is_sticky(void)
{
    fixture f;
    fixture_init(&f);
    f.owner.resolve_hole = NULL;
    const u32 base = 0x1000u;
    f.words[(base + 0u) >> 2] = packet(1u, 0x0004u);
    f.words[(base + 4u) >> 2] = 0xCAFEBABEu;
    u32 next = 0, ret = 0;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 8u, ~0u, &next, &ret) ==
          RSX_NR_FRAME_FATAL,
          "unsupported method did not fail");
    CHECK(f.owner.failure.kind ==
              RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD &&
              f.owner.failure.get == base &&
              f.owner.failure.method == 0x0004u &&
              f.owner.failure.argument == 0xCAFEBABEu &&
              f.adapter.methods_seen == 0u,
          "first unsupported record was not exact");
    f.words[(base + 4u) >> 2] = 0u;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 8u, ~0u, &next, &ret) ==
              RSX_NR_FRAME_FATAL &&
              f.owner.failure.argument == 0xCAFEBABEu,
          "fatal record was not sticky");
    return 0;
}

static int test_execution_failure_retains_exact_argument(void)
{
    fixture f;
    fixture_init(&f);
    const u32 base = 0x1000u;
    f.words[(base + 0u) >> 2] = packet(1u, 0xE944u);
    f.words[(base + 4u) >> 2] = 9u; /* fixture presenter accepts only 0..7 */
    u32 next = 0, ret = 0;
    CHECK(rsx_nr_frame_owner_step(
              &f.owner, base, base + 8u, ~0u, &next, &ret) ==
          RSX_NR_FRAME_FATAL,
          "backend execution refusal did not stop the strict owner");
    CHECK(f.owner.failure.kind == RSX_NR_FRAME_FAILURE_EXECUTION &&
              f.owner.failure.method == 0xE944u &&
              f.owner.failure.argument == 9u &&
              f.owner.failure.argument_index == 0u,
          "execution refusal lost its exact method argument");
    return 0;
}

int main(void)
{
    if (test_consume_once_and_present() ||
        test_semaphore_retry_is_not_retranslated() ||
        test_partial_stopper_and_flow() ||
        test_exact_published_head_stopper_release() ||
        test_call_return_and_exact_unmapped_failure() ||
        test_called_list_entry_waits_for_final_publication() ||
        test_jump_waits_for_exact_target_publication() ||
        test_invalid_jump_repair_is_exact_and_latched() ||
        test_publication_timeout_uses_wall_time_not_poll_rate() ||
        test_late_published_island_entry_preserves_payload() ||
        test_primary_generated_hole_proof_is_exact_and_latched() ||
        test_pending_generated_hole_rechecks_without_put_change() ||
        test_generated_hole_postproof_reread_retries() ||
        test_generated_hole_put_progress_resets_failure_bound() ||
        test_generated_hole_receives_owner_predecessor() ||
        test_primary_unmapped_flow_waits_for_in_place_publication() ||
        test_packet_shaped_generated_hole_executes_no_arguments() ||
        test_noop_preserves_sequential_gap_provenance() ||
        test_primary_hole_waits_but_called_list_fails() ||
        test_registered_island_edge_skips_payload_exactly() ||
        test_control_cycle_is_bounded() ||
        test_first_unsupported_is_sticky() ||
        test_execution_failure_retains_exact_argument())
        return 1;
    puts("rsx_nr_frame_owner: PASS");
    return 0;
}
