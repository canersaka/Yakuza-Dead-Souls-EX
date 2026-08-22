#include "ps3emu/yz_fe0_timeline.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); \
    ++failures; } } while (0)

static void disabled_is_inert(void)
{
    yz_fe0_timeline_test_reset(1000000u, 0);
    yz_fe0_timeline_emit(YZ_FE0_EVENT_UCMD_DISPATCH, 1, 2, 3, 4, 5, 6);
    yz_fe0_timeline_rsx_acquire(0x66616661u, 0xFE0u, 0x10200FE0u,
                                7u, 6u, 1);
    CHECK(yz_fe0_timeline_test_claimed() == 0);
    CHECK(yz_fe0_timeline_test_clock_reads() == 0);
    CHECK(yz_fe0_timeline_test_thread_time_reads() == 0);
    CHECK(yz_fe0_timeline_test_thread_cycle_reads() == 0);
}

static void semantic_order_is_retained(void)
{
    yz_fe0_timeline_test_reset(1000000u, 1);
    for (uint32_t type = YZ_FE0_EVENT_UCMD_DISPATCH;
         type <= YZ_FE0_EVENT_PUBLISHED; ++type) {
        yz_fe0_timeline_test_set_clock(100u + type);
        yz_fe0_timeline_emit((yz_fe0_event_type)type, 9u, type,
                             type + 1u, type + 2u, type + 3u, type + 4u);
    }
    CHECK(yz_fe0_timeline_test_claimed() == YZ_FE0_EVENT_PUBLISHED);
    for (uint64_t sequence = 1; sequence <= YZ_FE0_EVENT_PUBLISHED;
         ++sequence) {
        yz_fe0_timeline_record record;
        CHECK(yz_fe0_timeline_test_record(sequence, &record));
        CHECK(record.sequence == sequence);
        CHECK(record.type == sequence);
        CHECK(record.qpc == 100u + sequence);
        CHECK(record.cause == 9u);
    }
}

static void million_acquire_retries_are_one_episode(void)
{
    yz_fe0_timeline_test_reset(1000000u, 1);
    yz_fe0_timeline_test_set_clock(10u);
    for (unsigned i = 0; i < 1000000u; ++i)
        yz_fe0_timeline_rsx_acquire(0x66616661u, 0xFE0u,
                                    0x10200FE0u, 6u, 5u, 1);
    CHECK(yz_fe0_timeline_test_claimed() == 1);
    CHECK(yz_fe0_timeline_test_clock_reads() == 1);
    yz_fe0_timeline_test_set_clock(2010u);
    yz_fe0_timeline_rsx_acquire(0x66616661u, 0xFE0u,
                                0x10200FE0u, 6u, 6u, 0);
    CHECK(yz_fe0_timeline_test_claimed() == 2);
    CHECK(yz_fe0_timeline_test_clock_reads() == 2);
    yz_fe0_timeline_record wait, ready;
    CHECK(yz_fe0_timeline_test_record(1, &wait));
    CHECK(yz_fe0_timeline_test_record(2, &ready));
    CHECK(wait.type == YZ_FE0_EVENT_RSX_WAIT);
    CHECK(ready.type == YZ_FE0_EVENT_RSX_READY);
    CHECK(wait.cause == 6u && ready.cause == 6u);
    CHECK(ready.qpc - wait.qpc == 2000u);
}

static void task_handoff_retains_callsite(void)
{
    yz_fe0_timeline_test_reset(1000000u, 1);
    yz_fe0_timeline_test_set_clock(77u);
    yz_fe0_timeline_test_set_thread_time(1234u);
    yz_fe0_timeline_test_set_thread_cycles(5678u);
    yz_fe0_timeline_emit(YZ_FE0_EVENT_WKL4_HANDOFF,
                         0x42u, 3u, 9u, 4u, 2u, 0x5D34u);
    yz_fe0_timeline_record record;
    CHECK(yz_fe0_timeline_test_record(1u, &record));
    CHECK(record.type == YZ_FE0_EVENT_WKL4_HANDOFF);
    CHECK(record.cause == 0x42u && record.actor == 3u);
    CHECK(record.a0 == 9u && record.a1 == 4u);
    CHECK(record.a2 == 2u && record.a3 == 0x5D34u);
    CHECK(record.thread_time_100ns == 1234u);
    CHECK(record.thread_cycles == 5678u);
    CHECK(yz_fe0_timeline_test_clock_reads() == 1u);
    CHECK(yz_fe0_timeline_test_thread_time_reads() == 1u);
    CHECK(yz_fe0_timeline_test_thread_cycle_reads() == 1u);

    yz_fe0_timeline_test_set_clock(78u);
    yz_fe0_timeline_emit(YZ_FE0_EVENT_WKL4_RECORD,
                         0x42u, 3u, 9u, 4u, 2u, 0x5D34u);
    CHECK(yz_fe0_timeline_test_record(2u, &record));
    CHECK(record.thread_time_100ns == 0u);
    CHECK(record.thread_cycles == 0u);
    CHECK(yz_fe0_timeline_test_thread_time_reads() == 1u);
    CHECK(yz_fe0_timeline_test_thread_cycle_reads() == 1u);
}

static void key_change_starts_new_episode(void)
{
    yz_fe0_timeline_test_reset(1000000u, 1);
    yz_fe0_timeline_test_set_clock(1u);
    yz_fe0_timeline_rsx_acquire(1u, 0xFE0u, 0x10200FE0u, 1u, 0u, 1);
    yz_fe0_timeline_test_set_clock(2u);
    yz_fe0_timeline_rsx_acquire(1u, 0xFE0u, 0x10200FE0u, 2u, 0u, 1);
    CHECK(yz_fe0_timeline_test_claimed() == 2);
    CHECK(yz_fe0_timeline_test_clock_reads() == 2);
}

static void callback_correlation_has_a_bounded_lifetime(void)
{
    yz_fe0_timeline_test_reset(1000000u, 1);
    uint32_t cause = 0, epoch = 0;
    CHECK(!yz_fe0_timeline_callback_snapshot(&cause, &epoch));
    yz_fe0_timeline_test_set_clock(100u);
    yz_fe0_timeline_callback_begin(0x45u, 9u);
    CHECK(yz_fe0_timeline_callback_snapshot(&cause, &epoch));
    CHECK(cause == 0x45u && epoch == 9u);
    yz_fe0_timeline_test_set_clock(200u);
    yz_fe0_timeline_callback_end(0x45u, 9u);
    CHECK(!yz_fe0_timeline_callback_snapshot(&cause, &epoch));
    CHECK(yz_fe0_timeline_test_claimed() == 2u);
}

static void barrier_and_taskset_atomic_edges_are_exact(void)
{
    uint8_t before[128];
    uint8_t after[128];
    memset(before, 0, sizeof(before));
    memset(after, 0, sizeof(after));

    yz_fe0_timeline_test_reset(1000000u, 1);
    yz_fe0_timeline_set_wkl4_barrier(0x42452700u, 0x42450E00u);

    after[3] = 1u;
    yz_fe0_timeline_test_set_clock(10u);
    yz_fe0_timeline_observe_wkl4_atomic(
        4u, 0x2004u, 0xA780u, 0x61Cu, 3u, 0x42452700u,
        before, after);
    yz_fe0_timeline_record barrier;
    CHECK(yz_fe0_timeline_test_record(1u, &barrier));
    CHECK(barrier.type == YZ_FE0_EVENT_WKL4_BARRIER_WRITE);
    CHECK(barrier.cause == 0x61Cu);
    CHECK(barrier.actor == 0x20040003u);
    CHECK(barrier.a0 == 0xA780u);
    CHECK(barrier.a1 == 0u && barrier.a2 == 1u);

    memset(before, 0, sizeof(before));
    memset(after, 0, sizeof(after));
    after[0x40] = 0x80u;
    yz_fe0_timeline_test_set_clock(20u);
    yz_fe0_timeline_observe_wkl4_atomic(
        4u, 0x2005u, 0xAA48u, 0x61Cu, 4u, 0x42450E00u,
        before, after);
    yz_fe0_timeline_record signal;
    CHECK(yz_fe0_timeline_test_record(2u, &signal));
    CHECK(signal.type == YZ_FE0_EVENT_WKL4_TASKSET_WRITE);
    CHECK(signal.actor == 0x20050004u);
    CHECK(signal.a0 == 0xAA48u);
    CHECK(signal.a1 == 0x42450E00u);
    CHECK(signal.a2 == 0u && signal.a3 == 0x80000000u);

    yz_fe0_timeline_test_set_clock(30u);
    yz_fe0_timeline_observe_wkl4_atomic(
        4u, 0x2005u, 0xAA48u, 0x61Cu, 4u, 0x42453000u,
        before, after);
    CHECK(yz_fe0_timeline_test_claimed() == 2u);
    CHECK(yz_fe0_timeline_test_clock_reads() == 2u);

    yz_fe0_timeline_test_reset(1000000u, 0);
    yz_fe0_timeline_set_wkl4_barrier(0x42452700u, 0x42450E00u);
    yz_fe0_timeline_observe_wkl4_atomic(
        4u, 0x2005u, 0xAA48u, 0x61Cu, 4u, 0x42450E00u,
        before, after);
    CHECK(yz_fe0_timeline_test_claimed() == 0u);
    CHECK(yz_fe0_timeline_test_clock_reads() == 0u);
}

static void taskset_attempts_retain_fail_success_and_noop(void)
{
    uint8_t current[128];
    uint8_t candidate[128];
    memset(current, 0, sizeof(current));
    memset(candidate, 0, sizeof(candidate));
    candidate[0x30] = 0x80u;
    candidate[0x40] = 0x40u;
    candidate[0x50] = 0x20u;
    candidate[0x60] = 0x42u;
    candidate[0x61] = 0x40u;
    candidate[0x62] = 0x00u;
    candidate[0x63] = 0x00u;

    yz_fe0_timeline_test_reset(1000000u, 1);
    yz_fe0_timeline_set_wkl4_barrier(0x42452700u, 0x42450E00u);
    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        4u, 0x2004u, 0xAA48u, 0x61Cu, 2u, 0x42450E00u,
        0, current, candidate);
    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        4u, 0x2004u, 0xAA48u, 0x61Cu, 2u, 0x42450E00u,
        1, current, candidate);
    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        4u, 0x2004u, 0xAA48u, 0x61Cu, 2u, 0x42450E00u,
        1, candidate, candidate);
    CHECK(yz_fe0_timeline_test_taskset_attempts() == 3u);
    CHECK(yz_fe0_timeline_test_taskset_successes() == 2u);
    CHECK(yz_fe0_timeline_test_taskset_changes() == 2u);
    CHECK(yz_fe0_timeline_test_clock_reads() == 0u);

    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        4u, 0x2004u, 0xAA44u, 0x61Cu, 2u, 0x42450E00u,
        1, current, candidate);
    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        3u, 0x2004u, 0xAA48u, 0x61Cu, 2u, 0x42450E00u,
        1, current, candidate);
    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        4u, 0x2004u, 0xAA48u, 0x61Cu, 2u, 0x42453000u,
        1, current, candidate);
    CHECK(yz_fe0_timeline_test_taskset_attempts() == 3u);

    yz_fe0_timeline_test_reset(1000000u, 0);
    yz_fe0_timeline_set_wkl4_barrier(0x42452700u, 0x42450E00u);
    yz_fe0_timeline_observe_wkl4_taskset_attempt(
        4u, 0x2004u, 0xAA48u, 0x61Cu, 2u, 0x42450E00u,
        1, current, candidate);
    CHECK(yz_fe0_timeline_test_taskset_attempts() == 0u);
    CHECK(yz_fe0_timeline_test_clock_reads() == 0u);
}

int main(void)
{
    disabled_is_inert();
    semantic_order_is_retained();
    million_acquire_retries_are_one_episode();
    task_handoff_retains_callsite();
    key_change_starts_new_episode();
    callback_correlation_has_a_bounded_lifetime();
    barrier_and_taskset_atomic_edges_are_exact();
    taskset_attempts_retain_fail_success_and_noop();
    if (failures) {
        fprintf(stderr, "fe0 timeline tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("fe0 timeline tests: PASS\n");
    return 0;
}
