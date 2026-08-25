#include "ps3emu/yz_fifo_publication.h"

#include <stdio.h>

static int expect(const char* name, uint32_t actual, uint32_t wanted)
{
    if (actual == wanted)
        return 0;
    fprintf(stderr, "%s: got 0x%08X, wanted 0x%08X\n",
            name, actual, wanted);
    return 1;
}

int main(void)
{
    int failed = 0;
    const uint32_t base = 0x40400000u;
    const uint32_t size = 0x00800000u;

    failed |= expect("one startup head release",
        yz_fifo_startup_head_release_resume(
            0x00001000u, 0x00002000u, 0x20001000u,
            size, 0x00020000u, 0), 0x00001004u);
    failed |= expect("recycled startup head remains parked",
        yz_fifo_startup_head_release_resume(
            0x00001000u, 0x00002000u, 0x20001000u,
            size, 0x00020000u, 1), 0u);
    failed |= expect("startup head wrong command",
        yz_fifo_startup_head_release_resume(
            0x00001000u, 0x00002000u, 0x20001004u,
            size, 0x00020000u, 0), 0u);
    failed |= expect("startup head unpublished",
        yz_fifo_startup_head_release_resume(
            0x00001000u, 0x00001004u, 0x20001000u,
            size, 0x00020000u, 0), 0u);
    failed |= expect("nonzero segment head not host-released",
        yz_fifo_startup_head_release_resume(
            0x00020000u, 0x00021000u, 0x20020000u,
            size, 0x00020000u, 0), 0u);

    /* Keep the established stopper helper covered independently. */
    failed |= expect("captured island edge",
        yz_fifo_registered_island_resume(
            0x4074F180u, 0x4074FAA0u,
            0x0034F180u, 0x00011EE0u, base, size),
        0x0034FAA0u);

    failed |= expect("absent record",
        yz_fifo_registered_island_resume(
            0x4074F180u, 0u,
            0x0034F180u, 0x00011EE0u, base, size), 0u);
    failed |= expect("wrong stopper generation",
        yz_fifo_registered_island_resume(
            0x4074F184u, 0x4074FAA0u,
            0x0034F180u, 0x00011EE0u, base, size), 0u);
    failed |= expect("backward edge",
        yz_fifo_registered_island_resume(
            0x4074F180u, 0x4074F100u,
            0x0034F180u, 0x00011EE0u, base, size), 0u);
    failed |= expect("implausible span",
        yz_fifo_registered_island_resume(
            0x4074F180u, 0x4076F180u,
            0x0034F180u, 0x00011EE0u, base, size), 0u);
    failed |= expect("not yet published",
        yz_fifo_registered_island_resume(
            0x4074F180u, 0x4074FAA0u,
            0x0034F180u, 0x0034F900u, base, size), 0u);

    /* func_00EAB3DC writes a forward jump to the aligned payload and then
     * publishes the allocation end.  The payload is data, never a command
     * target for the strict native consumer. */
    failed |= expect("inline allocator edge",
        yz_fifo_registered_inline_island_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001000u, 0x00002000u, base, size),
        0x00001930u);
    failed |= expect("inline wrong command family",
        yz_fifo_registered_inline_island_resume(
            0x40401000u, 0x00041010u, 0x40401930u,
            0x00001000u, 0x00002000u, base, size), 0u);
    failed |= expect("inline distant target",
        yz_fifo_registered_inline_island_resume(
            0x40401000u, 0x20001100u, 0x40401930u,
            0x00001000u, 0x00002000u, base, size), 0u);
    failed |= expect("inline target past end",
        yz_fifo_registered_inline_island_resume(
            0x40401000u, 0x20001010u, 0x40401010u,
            0x00001000u, 0x00002000u, base, size), 0u);
    failed |= expect("inline not yet published",
        yz_fifo_registered_inline_island_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001000u, 0x00001900u, base, size), 0u);

    /* If the source zero was consumed before the exact edge became visible,
     * only the recorded payload start may recover to the same allocation end.
     * Interior words and incomplete publication remain fail-closed. */
    failed |= expect("late inline payload entry",
        yz_fifo_registered_inline_island_member_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001010u, 0x00002000u, base, size),
        0x00001930u);
    failed |= expect("late inline interior word",
        yz_fifo_registered_inline_island_member_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001014u, 0x00002000u, base, size), 0u);
    failed |= expect("late inline wrong recorded target",
        yz_fifo_registered_inline_island_member_resume(
            0x40401000u, 0x2000100Cu, 0x40401930u,
            0x00001010u, 0x00002000u, base, size), 0u);
    failed |= expect("late inline not yet published",
        yz_fifo_registered_inline_island_member_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001010u, 0x00001900u, base, size), 0u);
    failed |= expect("registered inline interior member",
        yz_fifo_registered_inline_island_interior_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001480u, 0x00002000u, base, size), 0x00001930u);
    failed |= expect("registered inline interior before payload",
        yz_fifo_registered_inline_island_interior_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x0000100Cu, 0x00002000u, base, size), 0u);
    failed |= expect("registered inline interior at resume",
        yz_fifo_registered_inline_island_interior_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001930u, 0x00002000u, base, size), 0u);
    failed |= expect("registered inline interior incomplete PUT",
        yz_fifo_registered_inline_island_interior_resume(
            0x40401000u, 0x20001010u, 0x40401930u,
            0x00001480u, 0x00001800u, base, size), 0u);

    /* Captured generated-list boundary: 17-argument VP constant packet at
     * 0x1230, two padding/data words at 0x1278, exact prologue at 0x1280. */
    failed |= expect("generated VP constant tail",
        yz_fifo_generated_vp_constant_tail_resume(
            0x00441EFCu, 0x3A2AAAABu,
            0x00001278u, 0x007FFFA0u, size, 1),
        0x00001280u);
    failed |= expect("generated VP wrong producer packet",
        yz_fifo_generated_vp_constant_tail_resume(
            0x00401EFCu, 0x3A2AAAABu,
            0x00001278u, 0x007FFFA0u, size, 1), 0u);
    failed |= expect("generated VP command-shaped tail",
        yz_fifo_generated_vp_constant_tail_resume(
            0x00441EFCu, 0x00041710u,
            0x00001278u, 0x007FFFA0u, size, 1), 0u);
    failed |= expect("generated VP flow-shaped tail",
        yz_fifo_generated_vp_constant_tail_resume(
            0x00441EFCu, 0x20001280u,
            0x00001278u, 0x007FFFA0u, size, 1), 0u);
    failed |= expect("generated VP missing prologue",
        yz_fifo_generated_vp_constant_tail_resume(
            0x00441EFCu, 0x3A2AAAABu,
            0x00001278u, 0x007FFFA0u, size, 0), 0u);
    failed |= expect("generated VP incomplete publication",
        yz_fifo_generated_vp_constant_tail_resume(
            0x00441EFCu, 0x3A2AAAABu,
        0x00001278u, 0x000012A8u, size, 1), 0u);

    failed |= expect("generated VP program tail",
        yz_fifo_generated_vp_program_tail_resume(
            0x000013B8u, 0x00800B80u, 0xFF00043Eu,
            0x0000143Cu, 0x00001450u, size, 7u,
            0x00041808u, 0u), 0x00001448u);
    failed |= expect("generated VP program tail short run",
        yz_fifo_generated_vp_program_tail_resume(
            0x000013B8u, 0x00800B80u, 0xFF00043Eu,
            0x0000143Cu, 0x00001450u, size, 3u,
            0x00041808u, 0u), 0u);
    failed |= expect("generated VP program tail mapped call",
        yz_fifo_generated_vp_program_tail_resume(
            0x000013B8u, 0x00800B80u, 0x0001043Eu,
            0x0000143Cu, 0x00001450u, size, 7u,
            0x00041808u, 0u), 0u);
    failed |= expect("generated VP program tail wrong end",
        yz_fifo_generated_vp_program_tail_resume(
            0x000013B8u, 0x00800B80u, 0xFF00043Eu,
            0x0000143Cu, 0x00001450u, size, 7u,
            0x00041808u, 1u), 0u);
    failed |= expect("generated VP program tail unpublished end",
        yz_fifo_generated_vp_program_tail_resume(
            0x000013B8u, 0x00800B80u, 0xFF00043Eu,
            0x0000143Cu, 0x0000144Cu, size, 7u,
            0x00041808u, 0u), 0u);

    /* Complete captured gap: NOOP at 0x1254, ten raw words at 0x1258,
     * generated draw prologue at 0x1280. */
    failed |= expect("generated VP complete inline gap",
        yz_fifo_generated_vp_inline_gap_resume(
            0x00001254u, 0u, 0x44C00000u,
            0x00001258u, 0x007FFFA0u, size, 1, 1),
        0x00001280u);
    failed |= expect("generated VP inline gap wrong predecessor",
        yz_fifo_generated_vp_inline_gap_resume(
            0x00001250u, 0u, 0x44C00000u,
            0x00001258u, 0x007FFFA0u, size, 1, 1), 0u);
    failed |= expect("generated VP inline gap real flow",
        yz_fifo_generated_vp_inline_gap_resume(
            0x00001254u, 0u, 0x20001280u,
            0x00001258u, 0x007FFFA0u, size, 1, 1), 0u);
    failed |= expect("generated VP inline gap missing prologue",
        yz_fifo_generated_vp_inline_gap_resume(
            0x00001254u, 0u, 0x44C00000u,
            0x00001258u, 0x007FFFA0u, size, 0, 1), 0u);
    failed |= expect("generated VP inline gap unbalanced",
        yz_fifo_generated_vp_inline_gap_resume(
            0x00001254u, 0u, 0x44C00000u,
            0x00001258u, 0x007FFFA0u, size, 1, 0), 0u);

    /* Warm-repeat capture: the same exact NOOP-owned generated-VP payload is
     * 0x38 bytes long. Its last raw word is itself a plausible one-argument
     * RT_ENABLE packet, so only the separately proven prologue at 0x1280 may
     * terminate the data island. */
    failed |= expect("generated VP variable inline gap",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x00001244u, 0u, 0x3F000000u,
            0x00001248u, 0x007FFFA0u, 0x00001280u,
            size, 0x100u, 1, 1, 1),
        0x00001280u);
    failed |= expect("generated VP variable gap wrong predecessor",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x00001240u, 0u, 0x3F000000u,
            0x00001248u, 0x007FFFA0u, 0x00001280u,
            size, 0x100u, 1, 1, 1), 0u);
    failed |= expect("generated VP variable gap too wide",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x00001244u, 0u, 0x3F000000u,
            0x00001248u, 0x007FFFA0u, 0x00001380u,
            size, 0x100u, 1, 1, 1), 0u);
    failed |= expect("generated VP variable gap flow word",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x00001244u, 0u, 0x20001280u,
            0x00001248u, 0x007FFFA0u, 0x00001280u,
            size, 0x100u, 0, 1, 1), 0u);
    failed |= expect("generated VP variable gap incomplete publication",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x00001244u, 0u, 0x3F000000u,
            0x00001248u, 0x000012A8u, 0x00001280u,
            size, 0x100u, 1, 1, 1), 0u);
    failed |= expect("generated VP variable gap unbalanced",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x00001244u, 0u, 0x3F000000u,
            0x00001248u, 0x007FFFA0u, 0x00001280u,
            size, 0x100u, 1, 1, 0), 0u);

    /* Frontier leg-2 capture: sequential NOOP at 0x47200C, a full
     * 0x100-byte producer-owned constant payload, then the exact generated
     * prologue at 0x472110. The prologue witness itself extends another
     * 0x2C bytes and must be published. */
    failed |= expect("generated VP 0x100 inline gap",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x0047200Cu, 0u, 0xBF800347u,
            0x00472010u, 0x00472180u, 0x00472110u,
            size, 0x200u, 0, 1, 1), 0x00472110u);
    failed |= expect("generated VP 0x100 gap incomplete witness",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x0047200Cu, 0u, 0xBF800347u,
            0x00472010u, 0x00472138u, 0x00472110u,
            size, 0x200u, 0, 1, 1), 0u);
    failed |= expect("generated VP 0x100 gap over bounded limit",
        yz_fifo_generated_vp_inline_candidate_resume(
            0x0047200Cu, 0u, 0xBF800347u,
            0x00472010u, 0x00472300u, 0x00472214u,
            size, 0x200u, 0, 1, 1), 0u);

    /* Orphanage startup capture: complete BEGIN_END(0) at 0x1074/0x1078,
     * a 0x204-byte vertex-program payload whose first word aliases an
     * unmapped JUMP, then the exact generated prologue at 0x1280. */
    failed |= expect("generated VP post-draw payload",
        yz_fifo_generated_vp_post_draw_candidate_resume(
            0x00001074u, 0x00041808u, 0u, 0x2041FFFCu,
            0x0000107Cu, 0x00001300u, 0x00001280u,
            size, 0x300u, 1, 1, 1), 0x00001280u);
    failed |= expect("generated VP post-draw wrong end command",
        yz_fifo_generated_vp_post_draw_candidate_resume(
            0x00001074u, 0x00041814u, 0u, 0x2041FFFCu,
            0x0000107Cu, 0x00001300u, 0x00001280u,
            size, 0x300u, 1, 1, 1), 0u);
    failed |= expect("generated VP post-draw nonzero end argument",
        yz_fifo_generated_vp_post_draw_candidate_resume(
            0x00001074u, 0x00041808u, 5u, 0x2041FFFCu,
            0x0000107Cu, 0x00001300u, 0x00001280u,
            size, 0x300u, 1, 1, 1), 0u);
    failed |= expect("generated VP post-draw mapped flow",
        yz_fifo_generated_vp_post_draw_candidate_resume(
            0x00001074u, 0x00041808u, 0u, 0x20001280u,
            0x0000107Cu, 0x00001300u, 0x00001280u,
            size, 0x300u, 0, 1, 1), 0u);
    failed |= expect("generated VP post-draw missing prologue",
        yz_fifo_generated_vp_post_draw_candidate_resume(
            0x00001074u, 0x00041808u, 0u, 0x2041FFFCu,
            0x0000107Cu, 0x00001300u, 0x00001280u,
            size, 0x300u, 1, 0, 1), 0u);
    failed |= expect("generated VP post-draw incomplete publication",
        yz_fifo_generated_vp_post_draw_candidate_resume(
            0x00001074u, 0x00041808u, 0u, 0x2041FFFCu,
            0x0000107Cu, 0x000012A8u, 0x00001280u,
            size, 0x300u, 1, 1, 1), 0u);

    /* Captured EDGE block boundary: a primary JUMP targets 0x41FFFC, the
     * producer-owned final word of a 128 KiB generated block.  Recycled float
     * data there may look like a packet; only the exact prologue and balanced
     * prefix at 0x420000 permit resumption. */
    failed |= expect("generated block tail",
        yz_fifo_generated_block_tail_resume(
            0x0041FFFCu, 0x43AC0000u, size, 0x00020000u, 1, 1),
        0x00420000u);
    failed |= expect("generated block real flow",
        yz_fifo_generated_block_tail_resume(
            0x0041FFFCu, 0x20420000u, size, 0x00020000u, 1, 1), 0u);
    failed |= expect("generated block wrong boundary",
        yz_fifo_generated_block_tail_resume(
            0x0041FFF8u, 0x43AC0000u, size, 0x00020000u, 1, 1), 0u);
    failed |= expect("generated block missing prologue",
        yz_fifo_generated_block_tail_resume(
            0x0041FFFCu, 0x43AC0000u, size, 0x00020000u, 0, 1), 0u);
    failed |= expect("generated block unbalanced",
        yz_fifo_generated_block_tail_resume(
            0x0041FFFCu, 0x43AC0000u, size, 0x00020000u, 1, 0), 0u);
    failed |= expect("generated block later exact candidate",
        yz_fifo_generated_block_candidate_resume(
            0x0041FFFCu, 0x43AC0000u, 0x00420180u,
            size, 0x00020000u, 1, 1), 0x00420180u);
    failed |= expect("generated block candidate outside next block",
        yz_fifo_generated_block_candidate_resume(
            0x0041FFFCu, 0x43AC0000u, 0x00440000u,
            size, 0x00020000u, 1, 1), 0u);
    failed |= expect("generated block later candidate with real flow tail",
        yz_fifo_generated_block_candidate_resume(
            0x0041FFFCu, 0x20420000u, 0x00420180u,
            size, 0x00020000u, 1, 1), 0u);
    failed |= expect("generated block later candidate missing prologue",
        yz_fifo_generated_block_candidate_resume(
            0x0041FFFCu, 0x43AC0000u, 0x00420180u,
            size, 0x00020000u, 0, 1), 0u);
    failed |= expect("generated block later candidate unbalanced",
        yz_fifo_generated_block_candidate_resume(
            0x0041FFFCu, 0x43AC0000u, 0x00420180u,
            size, 0x00020000u, 1, 0), 0u);

    if (!failed)
        puts("fifo publication island-edge regression: PASS");
    return failed ? 1 : 0;
}
