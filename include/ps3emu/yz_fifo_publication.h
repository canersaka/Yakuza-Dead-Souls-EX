#ifndef PS3EMU_YZ_FIFO_PUBLICATION_H
#define PS3EMU_YZ_FIFO_PUBLICATION_H

#include <stdint.h>

/* Validate a producer-recorded inline data-island edge without inspecting or
 * guessing through the island payload.  A nonzero result is the ring offset
 * of data_end_ea; zero means the record is not safe for publication repair. */
static inline uint32_t yz_fifo_registered_island_resume(
    uint32_t stopper_ea, uint32_t data_end_ea,
    uint32_t get, uint32_t put,
    uint32_t fifo_base, uint32_t fifo_size)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        (stopper_ea & 3u) || (data_end_ea & 3u) ||
        stopper_ea != fifo_base + (get & (fifo_size - 1u)) ||
        data_end_ea <= stopper_ea ||
        data_end_ea >= fifo_base + fifo_size)
        return 0u;

    /* func_00EAB3DC aligns a bounded allocation inside the FIFO.  Refuse a
     * stale/corrupt record that would skip an implausibly large region. */
    const uint32_t span = data_end_ea - stopper_ea;
    if (span < 4u || span > 0x10000u)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t resume = (data_end_ea - fifo_base) & mask;
    const uint32_t distance = (resume - get) & mask;
    const uint32_t ahead = (put - get) & mask;
    if (!resume || !ahead || distance != span || distance >= ahead)
        return 0u;
    return resume;
}

/* Validate the exact forward-JUMP/data/end triple published by the title's
 * inline FIFO allocator (func_00EAB3DC).  The command jumps from the current
 * command cursor to an aligned data payload; execution must resume at the
 * separately published allocation end, not at the payload bytes.  Keeping
 * this distinct from the older stopper helper above prevents an ambiguous
 * vertex-program word from being guessed as RSX control flow. */
static inline uint32_t yz_fifo_registered_inline_island_resume(
    uint32_t source_ea, uint32_t expected_command, uint32_t data_end_ea,
    uint32_t get, uint32_t put,
    uint32_t fifo_base, uint32_t fifo_size)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        (source_ea & 3u) || (data_end_ea & 3u) ||
        source_ea != fifo_base + (get & (fifo_size - 1u)) ||
        (expected_command & 0xE0000003u) != 0x20000000u)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t target = expected_command & 0x1FFFFFFCu;
    const uint32_t source = get & mask;
    const uint32_t resume = data_end_ea - fifo_base;
    if (data_end_ea <= source_ea || data_end_ea >= fifo_base + fifo_size ||
        target <= source || target >= resume)
        return 0u;

    /* The allocator aligns the payload immediately after the source word.
     * Refuse a record from another flow producer even if its command happens
     * to share the old-JUMP encoding. */
    const uint32_t prefix = target - source;
    const uint32_t span = resume - source;
    if (prefix < 4u || prefix > 0x10u || span < 8u || span > 0x10000u)
        return 0u;

    const uint32_t ahead = (put - get) & mask;
    if (!resume || !ahead || span >= ahead)
        return 0u;
    return resume;
}

/* Validate the title's exact generated VP-constant packet boundary.  The
 * producer emits one incrementing SET_TRANSFORM_CONSTANT_LOAD packet with a
 * load slot plus four float4 constants (17 arguments total), aligns the next
 * generated draw prologue to eight bytes, and leaves the intervening words as
 * data/padding.  A malformed word is never sufficient by itself: the complete
 * preceding packet identity, committed prologue extent, and independently
 * validated prologue must all agree before the cursor may advance.
 *
 * The caller re-reads the guest witnesses and PUT after a barrier.  Keeping
 * this pure helper separate makes the fail-closed layout proof deterministic
 * and prevents it from becoming a generic malformed-command skip. */
static inline uint32_t yz_fifo_generated_vp_constant_tail_resume(
    uint32_t previous_command, uint32_t tail_word,
    uint32_t get, uint32_t put, uint32_t fifo_size,
    int generated_prologue_ready)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        (get & 3u) || get >= fifo_size || put >= fifo_size ||
        previous_command != 0x00441EFCu ||
        generated_prologue_ready == 0)
        return 0u;

    /* Flow words and complete method headers must never be skipped.  This
     * exact live tail has non-method mask bits set; the following word is a
     * second producer-owned padding/data word and is revalidated by the
     * caller, not decoded independently. */
    const int flow =
        ((tail_word & 0xE0000003u) == 0x20000000u) ||
        ((tail_word & 3u) == 1u) || ((tail_word & 3u) == 2u) ||
        ((tail_word & 0xFFFF0003u) == 0x00020000u);
    const int method =
        (tail_word & 0xA0030003u) == 0u &&
        ((tail_word >> 18) & 0x7FFu) != 0u;
    if (tail_word == 0u || flow || method)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t resume = (get + 8u) & mask;
    const uint32_t ahead = (put - get) & mask;
    /* The exact prologue witness reads through resume+0x28 inclusive. */
    if (ahead < 0x34u || resume == 0u)
        return 0u;
    return resume;
}

/* Validate the EDGE generated-list block-boundary publication shape.  The
 * title links the primary FIFO to the final word of a fixed-size generated
 * block; that word is producer-owned flow storage and can still contain
 * recycled vertex/constant payload when the source JUMP becomes visible.
 * Execution may move to the following block only after the caller has proven
 * both the exact generated prologue and a complete draw-balanced prefix.
 *
 * This helper intentionally accepts packet-shaped recycled words: that is the
 * failure this boundary proof exists to distinguish.  A real flow word is
 * never bypassed, and no arbitrary scan result can satisfy the contract. */
static inline uint32_t yz_fifo_generated_block_tail_resume(
    uint32_t tail, uint32_t tail_word,
    uint32_t fifo_size, uint32_t block_size,
    int generated_prologue_ready, int balanced_prefix_ready)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        !block_size || (block_size & (block_size - 1u)) != 0u ||
        block_size > fifo_size || (tail & 3u) || tail >= fifo_size ||
        ((tail + 4u) & (block_size - 1u)) != 0u ||
        generated_prologue_ready == 0 || balanced_prefix_ready == 0)
        return 0u;

    const int flow =
        ((tail_word & 0xE0000003u) == 0x20000000u) ||
        ((tail_word & 3u) == 1u) || ((tail_word & 3u) == 2u) ||
        ((tail_word & 0xFFFF0003u) == 0x00020000u);
    if (flow)
        return 0u;

    const uint32_t resume = (tail + 4u) & (fifo_size - 1u);
    return resume ? resume : 0u;
}

#endif
