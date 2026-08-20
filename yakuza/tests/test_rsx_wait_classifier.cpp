#include "../rsx_wait_classifier.h"

#include <chrono>
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures;                                                          \
    }                                                                        \
} while (0)

static yz_rsx_semaphore_wait sem(uint32_t address, uint32_t wanted,
                                 uint32_t observed, uint32_t dma = 0x66616661u,
                                 uint32_t offset = 0xFE0u)
{
    return {dma, offset, address, wanted, observed};
}

static yz_rsx_stopper_wait stopper(uint32_t get, uint32_t word,
                                   uint32_t put, uint32_t ahead)
{
    return {get, 0x30000000u + get, put, ahead, word};
}

static void test_category_contract(void)
{
    static const char* expected[YZ_RSX_WAIT_CATEGORY_COUNT] = {
        "ADVANCING", "WAIT_EMPTY", "WAIT_PARTIAL_PACKET",
        "WAIT_SELF_STOPPER", "WAIT_SEMAPHORE", "WAIT_UNFINALIZED_HOLE",
        "WAIT_BAD_FLOW", "WAIT_NO_CONTEXT"
    };
    for (uint32_t i = 0; i < YZ_RSX_WAIT_CATEGORY_COUNT; ++i)
        CHECK(std::strcmp(yz_rsx_wait_category_name(
            (yz_rsx_wait_category)i), expected[i]) == 0);
}

static void test_disabled_is_inert(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 1000, 0);
    yz_rsx_wait_classifier_test_set_clock(2000);
    yz_rsx_wait_classifier_transition(YZ_RSX_WAIT_ADVANCING);
    yz_rsx_semaphore_wait s = sem(0x10200FE0u, 7, 3);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
    yz_rsx_stopper_wait p = stopper(0x1000, 0x20001000, 0x2000, 0x1000);
    yz_rsx_wait_classifier_stopper_observe(&p, 1);
    yz_rsx_wait_classifier_record(YZ_RSX_WAIT_SEMAPHORE, 4, 2, 8, 1);
    yz_rsx_wait_classifier_test_shutdown();
    yz_rsx_wait_test_bucket bucket = {};
    yz_rsx_wait_test_semaphore_aggregate aggregate = {};
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 0);
    CHECK(!yz_rsx_wait_classifier_test_bucket(0, &bucket));
    CHECK(!yz_rsx_wait_classifier_test_find_semaphore(&s, &aggregate));
}

static void test_million_retries_are_one_episode(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 1000, 1);
    yz_rsx_wait_classifier_test_set_clock(1000);
    yz_rsx_wait_classifier_transition(YZ_RSX_WAIT_ADVANCING);

    yz_rsx_semaphore_wait s = sem(0x10200FE0u, 9, 3);
    yz_rsx_wait_classifier_test_set_clock(1100);
    for (uint32_t i = 0; i < 1000000u; ++i) {
        yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
        yz_rsx_wait_classifier_record(YZ_RSX_WAIT_SEMAPHORE, 1, 0, 0x2000, 1);
    }
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 2);

    s.observed = 9;
    yz_rsx_wait_classifier_test_set_clock(2100);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 0);
    yz_rsx_wait_classifier_record(YZ_RSX_WAIT_ADVANCING, 1, 1, 0x2004, 1);
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 3);

    yz_rsx_wait_test_semaphore_aggregate a = {};
    CHECK(yz_rsx_wait_classifier_test_find_semaphore(&s, &a));
    CHECK(a.episode_count == 1);
    CHECK(a.poll_count == 1000000u);
    CHECK(a.total_ticks == 1000);
    CHECK(a.first_entry_value == 3);
    CHECK(a.last_exit_value == 9);
    CHECK(a.value_change_count == 1);

    yz_rsx_wait_test_bucket b0 = {};
    yz_rsx_wait_test_bucket b1 = {};
    CHECK(yz_rsx_wait_classifier_test_bucket(0, &b0));
    CHECK(yz_rsx_wait_classifier_test_bucket(1, &b1));
    CHECK(b0.transitions[YZ_RSX_WAIT_SEMAPHORE] == 1);
    CHECK(b1.transitions[YZ_RSX_WAIT_ADVANCING] == 1);
    CHECK(b0.transitions[YZ_RSX_WAIT_ADVANCING] == 0);
}

static void test_satisfied_before_wait_is_not_an_episode(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 0, 1);
    yz_rsx_semaphore_wait s = sem(0x10200030u, 5, 5);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 0);
    yz_rsx_wait_test_semaphore_aggregate a = {};
    CHECK(!yz_rsx_wait_classifier_test_find_semaphore(&s, &a));
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 0);
}

static void test_satisfaction_during_episode_registration(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 0, 1);
    /* The method's first read failed, but the post-method observation already
     * sees the writer's value.  The unchanged GET still makes this one real
     * wait episode; the next successful retry closes it without alternation. */
    yz_rsx_semaphore_wait s = sem(0x10200FE0u, 12, 12);
    yz_rsx_wait_classifier_test_set_clock(100);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
    yz_rsx_wait_classifier_test_set_clock(120);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 0);
    yz_rsx_wait_classifier_transition(YZ_RSX_WAIT_ADVANCING);
    yz_rsx_wait_test_semaphore_aggregate a = {};
    CHECK(yz_rsx_wait_classifier_test_find_semaphore(&s, &a));
    CHECK(a.episode_count == 1 && a.poll_count == 1);
    CHECK(a.first_entry_value == 12 && a.last_exit_value == 12);
    CHECK(a.total_ticks == 20 && a.value_change_count == 0);
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 2);
}

static void test_value_and_key_changes(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 0, 1);
    yz_rsx_semaphore_wait a = sem(0x10200030u, 10, 1);
    yz_rsx_wait_classifier_test_set_clock(10);
    yz_rsx_wait_classifier_semaphore_attempt(&a, 1);
    a.observed = 2;
    yz_rsx_wait_classifier_semaphore_attempt(&a, 1);
    a.observed = 4;
    yz_rsx_wait_classifier_semaphore_attempt(&a, 1);

    yz_rsx_semaphore_wait b = sem(0x10200034u, 11, 6,
                                  0x66616661u, 0x34u);
    yz_rsx_wait_classifier_test_set_clock(30);
    yz_rsx_wait_classifier_semaphore_attempt(&b, 1);
    yz_rsx_wait_classifier_test_set_clock(80);
    b.observed = 11;
    yz_rsx_wait_classifier_semaphore_attempt(&b, 0);
    yz_rsx_wait_classifier_transition(YZ_RSX_WAIT_ADVANCING);

    yz_rsx_wait_test_semaphore_aggregate aa = {};
    yz_rsx_wait_test_semaphore_aggregate bb = {};
    CHECK(yz_rsx_wait_classifier_test_find_semaphore(&a, &aa));
    CHECK(yz_rsx_wait_classifier_test_find_semaphore(&b, &bb));
    CHECK(aa.episode_count == 1 && aa.poll_count == 3);
    CHECK(aa.value_change_count == 2 && aa.total_ticks == 20);
    CHECK(bb.episode_count == 1 && bb.poll_count == 1);
    CHECK(bb.value_change_count == 1 && bb.total_ticks == 50);
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 3);
}

static void test_stopper_patch_and_put_changes(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 0, 1);
    yz_rsx_stopper_wait s = stopper(0x1000, 0x20001000u, 0x1004, 4);
    yz_rsx_wait_classifier_test_set_clock(100);
    yz_rsx_wait_classifier_stopper_observe(&s, 1);
    yz_rsx_wait_classifier_stopper_observe(&s, 1);
    s.put = 0x1100;
    s.put_ahead = 0x100;
    yz_rsx_wait_classifier_stopper_observe(&s, 1);
    s.word = 0x20001004u;
    yz_rsx_wait_classifier_stopper_observe(&s, 0);
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 1);
    yz_rsx_wait_classifier_test_set_clock(600);
    yz_rsx_wait_classifier_transition(YZ_RSX_WAIT_ADVANCING);

    yz_rsx_wait_test_stopper_aggregate a = {};
    CHECK(yz_rsx_wait_classifier_test_find_stopper(&s, &a));
    CHECK(a.episode_count == 1 && a.poll_count == 3);
    CHECK(a.word_change_count == 1 && a.put_change_count == 1);
    CHECK(a.last_exit_word == 0x20001004u);
    CHECK(a.last_exit_put == 0x1100u && a.last_exit_ahead == 0x100u);
    CHECK(a.total_ticks == 500);
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 2);
}

static void test_bucket_rollover_and_shutdown(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 0, 1);
    yz_rsx_wait_classifier_test_set_second(YZ_RSX_WAIT_TEST_BUCKET_CAP + 7u);
    yz_rsx_wait_classifier_test_set_clock(10);
    yz_rsx_wait_classifier_record(YZ_RSX_WAIT_ADVANCING, 3, 0, 0, 0);
    yz_rsx_wait_test_bucket b = {};
    CHECK(!yz_rsx_wait_classifier_test_bucket(7, &b));
    CHECK(yz_rsx_wait_classifier_test_bucket(
        YZ_RSX_WAIT_TEST_BUCKET_CAP + 7u, &b));
    CHECK(b.progressing_steps == 1 && b.dispatched_methods == 3);

    yz_rsx_semaphore_wait s = sem(0x10200FE0u, 4, 0);
    yz_rsx_wait_classifier_test_set_clock(100);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
    yz_rsx_wait_classifier_test_set_clock(300);
    yz_rsx_wait_classifier_test_shutdown();
    yz_rsx_wait_test_semaphore_aggregate a = {};
    CHECK(yz_rsx_wait_classifier_test_find_semaphore(&s, &a));
    CHECK(a.total_ticks == 200 && a.episode_count == 1);
    const uint64_t reads = yz_rsx_wait_classifier_test_clock_reads();
    yz_rsx_wait_classifier_test_shutdown();
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == reads);
}

static void test_fixed_table_saturation_stays_clock_bounded(void)
{
    yz_rsx_wait_classifier_test_reset(1000, 0, 1);
    for (uint32_t i = 0; i < 513u; ++i) {
        yz_rsx_wait_classifier_test_set_clock(i + 1u);
        yz_rsx_semaphore_wait s = sem(
            0x20000000u + i * 8u, i + 1u, i, 0xFEED0001u, i * 8u);
        yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
    }
    CHECK(yz_rsx_wait_classifier_test_semaphore_overflow() == 1);
    const uint64_t reads = yz_rsx_wait_classifier_test_clock_reads();
    yz_rsx_semaphore_wait overflow = sem(
        0x20000000u + 512u * 8u, 513u, 512u,
        0xFEED0001u, 512u * 8u);
    for (uint32_t i = 0; i < 1000000u; ++i)
        yz_rsx_wait_classifier_semaphore_attempt(&overflow, 1);
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == reads);
    CHECK(yz_rsx_wait_classifier_test_semaphore_overflow() == 1);
}

static void benchmark_enabled_retry_path(void)
{
    yz_rsx_wait_classifier_test_reset(1000000, 0, 1);
    yz_rsx_semaphore_wait s = sem(0x10200FE0u, 9, 3);
    yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
    constexpr uint32_t iterations = 5000000u;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iterations; ++i)
        yz_rsx_wait_classifier_semaphore_attempt(&s, 1);
    const auto end = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - begin).count();
    std::printf("rsx wait classifier enabled retry benchmark: %.2f ns/attempt "
                "(%u attempts, %llu clock reads)\n",
                ns / iterations, iterations,
                (unsigned long long)yz_rsx_wait_classifier_test_clock_reads());
    CHECK(yz_rsx_wait_classifier_test_clock_reads() == 1);
}

int main(void)
{
    test_category_contract();
    test_disabled_is_inert();
    test_million_retries_are_one_episode();
    test_satisfied_before_wait_is_not_an_episode();
    test_satisfaction_during_episode_registration();
    test_value_and_key_changes();
    test_stopper_patch_and_put_changes();
    test_bucket_rollover_and_shutdown();
    test_fixed_table_saturation_stays_clock_bounded();
    benchmark_enabled_retry_path();
    if (failures) {
        std::fprintf(stderr, "rsx wait classifier: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("rsx wait classifier: all tests passed");
    return 0;
}
