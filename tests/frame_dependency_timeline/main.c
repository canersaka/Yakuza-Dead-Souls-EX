#include "ps3emu/yz_frame_dependency_timeline.h"

#include <stdio.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
    __FILE__, __LINE__, #x); ++failures; } } while (0)

static yz_frame_dep_record record(uint64_t n)
{
    yz_frame_dep_record value = {0};
    CHECK(yz_frame_dependency_test_record(n, &value));
    return value;
}

static void disabled_path_is_inert(void)
{
    yz_frame_dependency_test_reset(1000000u, 0);
    yz_frame_dependency_test_set_clock(10u);
    CHECK(yz_frame_dep_ppu_update_start(0xD1E838u, 0u) == 0u);
    yz_frame_dep_spu_job_start(4u, 1u, 4u, 0x1000u);
    yz_frame_dep_dma_publish(4u, 1u, 0x3040u, 0x2000u, 4u, 0x20u);
    yz_frame_dep_fifo_publish(0x10u, 0x20u, 1u, 0u);
    yz_frame_dep_rsx_consume(0x10u, 0x20u, 0x20u, 1u);
    yz_frame_dep_present(0u, 1u, 1u);
    CHECK(yz_frame_dependency_test_claimed() == 0u);
    CHECK(yz_frame_dependency_test_clock_reads() == 0u);
}

static void exact_semantic_chain(void)
{
    uint64_t frame, wait;
    yz_frame_dep_record value;
    yz_frame_dependency_test_reset(1000000u, 1);
    yz_frame_dependency_test_set_clock(100u);
    frame = yz_frame_dep_ppu_update_start(0xD1E838u, 0x1234u);
    CHECK(frame == 1u);
    yz_frame_dependency_test_set_clock(110u);
    yz_frame_dep_spurs_schedule(2u, 4u, 4u, 7u);
    yz_frame_dependency_test_set_clock(120u);
    yz_frame_dep_spu_job_start(4u, 3u, 4u, 0x401ACB00u);
    yz_frame_dependency_test_set_clock(130u);
    wait = yz_frame_dep_ppu_wait_enter(0x10200FE0u, 7u);
    CHECK(wait != 0u);
    /* Unrelated and partial DMA writes take no timestamps. */
    yz_frame_dep_dma_publish(4u, 3u, 0x4f00u, 0x10202000u, 4u, 0x20u);
    yz_frame_dep_dma_publish(4u, 3u, 0x4f00u, 0x10200FE0u, 2u, 0x20u);
    CHECK(yz_frame_dependency_test_claimed() == 4u);
    yz_frame_dependency_test_set_clock(140u);
    yz_frame_dep_dma_publish(4u, 3u, 0x4f00u, 0x10200FC0u, 0x80u, 0x20u);
    yz_frame_dependency_test_set_clock(150u);
    yz_frame_dep_spu_job_complete(4u, 3u, 4u, 0x401ACB00u);
    yz_frame_dependency_test_set_clock(160u);
    yz_frame_dep_ppu_wait_exit(wait, 0x10200FE0u, 8u);
    yz_frame_dependency_test_set_clock(165u);
    yz_frame_dep_ppu_update_complete(frame, 0xD1E838u, 0u);
    yz_frame_dependency_test_set_clock(170u);
    yz_frame_dep_fifo_publish(0x200u, 0x300u, 1u, 0u);
    yz_frame_dep_rsx_consume(0x200u, 0x200u, 0x300u, 1u);
    CHECK(yz_frame_dependency_test_claimed() == 9u);
    yz_frame_dependency_test_set_clock(180u);
    yz_frame_dep_rsx_consume(0x200u, 0x240u, 0x300u, 1u);
    yz_frame_dep_rsx_consume(0x240u, 0x280u, 0x300u, 1u);
    yz_frame_dependency_test_set_clock(190u);
    yz_frame_dep_submission(5u, 1u, 9u);
    yz_frame_dependency_test_set_clock(200u);
    yz_frame_dep_frame_complete(1u, 1u);
    yz_frame_dependency_test_set_clock(210u);
    yz_frame_dep_present(1u, 1u, 1u);
    CHECK(yz_frame_dependency_test_claimed() == 13u);
    CHECK(yz_frame_dependency_test_clock_reads() == 13u);
    value = record(1u);
    CHECK(value.type == YZ_FRAME_DEP_PPU_UPDATE_START &&
          value.frame_generation == 1u);
    value = record(5u);
    CHECK(value.type == YZ_FRAME_DEP_DMA_PUBLISH &&
          value.dependency_generation == (wait >> 8));
    value = record(10u);
    CHECK(value.type == YZ_FRAME_DEP_RSX_CONSUME &&
          value.dependency_generation == (wait >> 8));
}

static void jobs_may_span_present(void)
{
    yz_frame_dependency_test_reset(1000000u, 1);
    yz_frame_dep_ppu_update_start(1u, 0u);
    yz_frame_dep_spu_job_start(6u, 2u, 3u, 0x1000u);
    yz_frame_dep_present(0u, 1u, 1u);
    yz_frame_dep_ppu_update_start(1u, 0u);
    yz_frame_dep_spu_job_complete(6u, 2u, 3u, 0x1000u);
    CHECK(record(2u).frame_generation == 1u);
    CHECK(record(5u).frame_generation == 2u);
    CHECK(record(2u).a3 == record(5u).a3);
}

int main(void)
{
    disabled_path_is_inert();
    exact_semantic_chain();
    jobs_may_span_present();
    if (failures) return 1;
    puts("frame dependency timeline checks passed");
    return 0;
}
