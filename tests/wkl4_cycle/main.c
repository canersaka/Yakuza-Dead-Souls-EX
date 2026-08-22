#include "ps3emu/yz_wkl4_cycle.h"
#include "ps3emu/yz_wkl4_cycle_interval.h"

#include <stdio.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); \
    ++failures; } } while (0)

static void disabled_is_inert(void)
{
    yz_wkl4_cycle_test_reset(0);
    yz_wkl4_cycle_test_set_clock(10u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_7E50_SETUP);
    yz_wkl4_cycle_leave();
    CHECK(yz_wkl4_cycle_test_clock_reads() == 0u);
    CHECK(yz_wkl4_cycle_test_entries(YZ_WKL4_CYCLE_7E50_SETUP) == 0u);
}

static void repeated_loop_marks_do_not_read_the_clock(void)
{
    yz_wkl4_cycle_test_reset(1);
    yz_wkl4_cycle_test_set_clock(100u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_7E50_LOOP);
    for (unsigned i = 0; i < 1000000u; ++i)
        yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_7E50_LOOP);
    CHECK(yz_wkl4_cycle_test_clock_reads() == 1u);
    CHECK(yz_wkl4_cycle_test_entries(YZ_WKL4_CYCLE_7E50_LOOP) == 1u);
    yz_wkl4_cycle_test_set_clock(1100u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_7E50_TAIL);
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_7E50_LOOP) == 1000u);
    CHECK(yz_wkl4_cycle_test_clock_reads() == 2u);
    yz_wkl4_cycle_test_set_clock(1300u);
    yz_wkl4_cycle_leave();
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_7E50_TAIL) == 200u);
    CHECK(yz_wkl4_cycle_test_clock_reads() == 3u);
}

static void region_change_closes_the_previous_segment(void)
{
    yz_wkl4_cycle_test_reset(1);
    yz_wkl4_cycle_test_set_clock(20u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8230_SETUP);
    yz_wkl4_cycle_test_set_clock(70u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8230_LOOP);
    yz_wkl4_cycle_test_set_clock(170u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8680_SETUP);
    yz_wkl4_cycle_test_set_clock(210u);
    yz_wkl4_cycle_leave();
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_8230_SETUP) == 50u);
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_8230_LOOP) == 100u);
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_8680_SETUP) == 40u);
}

static void hot_loop_subphases_are_separate(void)
{
    yz_wkl4_cycle_test_reset(1);
    yz_wkl4_cycle_test_set_clock(100u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8230_LOOP);
    yz_wkl4_cycle_test_set_clock(180u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8230_COMPARE);
    yz_wkl4_cycle_test_set_clock(300u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8230_STORE);
    yz_wkl4_cycle_test_set_clock(350u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_8230_LOOP);
    yz_wkl4_cycle_test_set_clock(440u);
    yz_wkl4_cycle_leave();
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_8230_LOOP) == 170u);
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_8230_COMPARE) == 120u);
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_8230_STORE) == 50u);
    CHECK(yz_wkl4_cycle_test_entries(YZ_WKL4_CYCLE_8230_LOOP) == 2u);
    CHECK(yz_wkl4_cycle_test_entries(YZ_WKL4_CYCLE_8230_COMPARE) == 1u);
    CHECK(yz_wkl4_cycle_test_entries(YZ_WKL4_CYCLE_8230_STORE) == 1u);
    CHECK(yz_wkl4_cycle_test_clock_reads() == 5u);
}

static void interval_boundary_excludes_prior_work(void)
{
    yz_wkl4_cycle_test_reset(1);
    yz_wkl4_cycle_test_set_clock(100u);
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_7E50_LOOP);
    yz_wkl4_cycle_test_set_clock(900u);
    yz_wkl4_cycle_begin_interval();
    yz_wkl4_cycle_mark(YZ_WKL4_CYCLE_7E50_LOOP);
    yz_wkl4_cycle_test_set_clock(1400u);
    yz_wkl4_cycle_leave();
    CHECK(yz_wkl4_cycle_test_cycles(YZ_WKL4_CYCLE_7E50_LOOP) == 500u);
    CHECK(yz_wkl4_cycle_test_entries(YZ_WKL4_CYCLE_7E50_LOOP) == 1u);
    CHECK(yz_wkl4_cycle_test_clock_reads() == 3u);
}

int main(void)
{
    disabled_is_inert();
    repeated_loop_marks_do_not_read_the_clock();
    region_change_closes_the_previous_segment();
    hot_loop_subphases_are_separate();
    interval_boundary_excludes_prior_work();
    if (failures) {
        fprintf(stderr, "wkl4 cycle tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("wkl4 cycle tests: PASS");
    return 0;
}
