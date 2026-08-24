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
                            &f->ring, read_word, f);
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
        test_call_return_and_exact_unmapped_failure() ||
        test_control_cycle_is_bounded() ||
        test_first_unsupported_is_sticky() ||
        test_execution_failure_retains_exact_argument())
        return 1;
    puts("rsx_nr_frame_owner: PASS");
    return 0;
}
