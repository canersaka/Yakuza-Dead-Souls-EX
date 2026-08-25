#ifndef PS3EMU_YZ_FIFO_PUBLICATION_H
#define PS3EMU_YZ_FIFO_PUBLICATION_H

#include <stdint.h>

/* The default FIFO's reserved segment-zero head has one process-lifetime
 * startup guard.  PUT may release that guard exactly once; after the ring is
 * recycled, a self-jump at the same address belongs to the ordinary producer
 * journal and cannot be advanced from PUT alone. */
static inline uint32_t yz_fifo_startup_head_release_resume(
    uint32_t get, uint32_t put, uint32_t command,
    uint32_t fifo_size, uint32_t segment_size,
    int already_released)
{
    if (already_released || !fifo_size ||
        (fifo_size & (fifo_size - 1u)) != 0u ||
        !segment_size || (segment_size & 3u) != 0u ||
        segment_size > fifo_size || get >= fifo_size || put >= fifo_size)
        return 0u;

    const uint32_t segment = get / segment_size;
    const uint32_t segment_head =
        segment * segment_size + (segment == 0u ? 0x1000u : 0u);
    const uint32_t ahead = (put - get) & (fifo_size - 1u);
    if (segment != 0u || get != segment_head ||
        command != (0x20000000u | get) ||
        ahead <= 4u || ahead >= (fifo_size >> 1))
        return 0u;
    return (get + 4u) & (fifo_size - 1u);
}

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

/* Recover when the consumer observed the allocator's source word while it was
 * still zero padding, advanced once, and only then saw the producer publish
 * the exact jump-over-payload edge.  member_get must be the recorded payload
 * start itself: this is not an interval search and cannot skip an arbitrary
 * word found inside a data allocation.  The caller obtains the source/end
 * triple from the producer record and revalidates that record, the source
 * word, the current member word, and PUT after a barrier. */
static inline uint32_t yz_fifo_registered_inline_island_member_resume(
    uint32_t source_ea, uint32_t expected_command, uint32_t data_end_ea,
    uint32_t member_get, uint32_t put,
    uint32_t fifo_base, uint32_t fifo_size)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        source_ea < fifo_base || source_ea >= fifo_base + fifo_size)
        return 0u;

    const uint32_t source_get = source_ea - fifo_base;
    const uint32_t resume = yz_fifo_registered_inline_island_resume(
        source_ea, expected_command, data_end_ea, source_get, put,
        fifo_base, fifo_size);
    if (!resume)
        return 0u;

    const uint32_t target = expected_command & 0x1FFFFFFCu;
    if (member_get != target || member_get <= source_get ||
        member_get >= resume)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t remaining = (resume - member_get) & mask;
    const uint32_t ahead = (put - member_get) & mask;
    return remaining && ahead && remaining < ahead ? resume : 0u;
}

/* Recover an owner that has advanced beyond the exact payload start because
 * one or more raw payload words happened to decode as supported commands.
 * This is still producer-record based, not a byte-pattern skip: the complete
 * source/JUMP/end triple supplies the allocation interval, and the caller
 * revalidates its generation, source word, current member word, and PUT after
 * a barrier.  Keep this separate from the fast exact-start query above so
 * ordinary FIFO traffic never scans for an owning interval. */
static inline uint32_t yz_fifo_registered_inline_island_interior_resume(
    uint32_t source_ea, uint32_t expected_command, uint32_t data_end_ea,
    uint32_t member_get, uint32_t put,
    uint32_t fifo_base, uint32_t fifo_size)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        source_ea < fifo_base || source_ea >= fifo_base + fifo_size)
        return 0u;

    const uint32_t source_get = source_ea - fifo_base;
    const uint32_t resume = yz_fifo_registered_inline_island_resume(
        source_ea, expected_command, data_end_ea, source_get, put,
        fifo_base, fifo_size);
    if (!resume)
        return 0u;

    const uint32_t target = expected_command & 0x1FFFFFFCu;
    if (member_get < target || member_get >= resume)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t remaining = (resume - member_get) & mask;
    const uint32_t ahead = (put - member_get) & mask;
    return remaining && ahead && remaining < ahead ? resume : 0u;
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

/* Validate the exact generated vertex-program upload tail seen on the
 * Frontier transition. The producer emits a run of 32-word incrementing
 * SET_TRANSFORM_PROGRAM packets (0x00800B80), leaves one final three-word
 * instruction/data fragment, then publishes SET_BEGIN_END(0). The fragment
 * is not FIFO: its first word can alias an absolute CALL by chance.
 *
 * Admission therefore requires the owner's exact preceding packet boundary,
 * at least four independently reread consecutive upload packets, an unmapped
 * CALL-shaped first data word, the exact draw-end pair, and PUT covering the
 * whole pair. The caller repeats every witness after a barrier. */
static inline uint32_t yz_fifo_generated_vp_program_tail_resume(
    uint32_t previous_get, uint32_t previous_command,
    uint32_t tail_word, uint32_t get, uint32_t put, uint32_t fifo_size,
    uint32_t consecutive_program_packets,
    uint32_t end_command, uint32_t end_argument)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        (previous_get & 3u) || previous_get >= fifo_size ||
        (get & 3u) || get >= fifo_size || put >= fifo_size ||
        previous_command != 0x00800B80u ||
        ((previous_get + 0x84u) & (fifo_size - 1u)) != get ||
        consecutive_program_packets < 4u ||
        (tail_word & 3u) != 2u ||
        (tail_word & 0x1FFFFFFCu) < fifo_size ||
        end_command != 0x00041808u || end_argument != 0u)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t resume = (get + 0x0Cu) & mask;
    const uint32_t ahead = (put - get) & mask;
    return resume && ahead >= 0x14u ? resume : 0u;
}

/* Validate a complete generated-VP inline data gap.  EDGE leaves one primary
 * ring NOOP immediately before a bounded producer-owned payload, followed by
 * the next exact generated draw prologue.  The payload length varies with the
 * vertex program (captured legal spans are 0x28, 0x38, and 0x100 bytes), and its
 * words can satisfy the method-header mask by chance.  Packet shape is never
 * an ownership proof: admission requires the exact sequential NOOP boundary,
 * the first independently identified prologue within max_gap, and a complete
 * draw-balanced prefix at that candidate. Real flow words are never bypassed.
 *
 * The caller finds the first exact prologue, proves its command chain, then
 * re-reads all witnesses and repeats this pure proof after a barrier. */
static inline uint32_t yz_fifo_generated_vp_inline_candidate_resume(
    uint32_t previous_get, uint32_t previous_command,
    uint32_t gap_word, uint32_t get, uint32_t put, uint32_t candidate,
    uint32_t fifo_size, uint32_t max_gap,
    int flow_word_unmapped,
    int generated_prologue_ready, int balanced_prefix_ready)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        (previous_get & 3u) || previous_get >= fifo_size ||
        (get & 3u) || get >= fifo_size || put >= fifo_size ||
        (candidate & 3u) || candidate >= fifo_size ||
        !max_gap || max_gap >= (fifo_size >> 1) ||
        previous_command != 0u ||
        ((previous_get + 4u) & (fifo_size - 1u)) != get ||
        generated_prologue_ready == 0 || balanced_prefix_ready == 0)
        return 0u;

    const int flow =
        ((gap_word & 0xE0000003u) == 0x20000000u) ||
        ((gap_word & 3u) == 1u) || ((gap_word & 3u) == 2u) ||
        ((gap_word & 0xFFFF0003u) == 0x00020000u);
    /* A flow-shaped payload is admissible only when the strict owner has
     * independently established that its encoded target is outside the FIFO
     * address space. A real mapped JUMP/CALL is never bypassed. */
    if (!gap_word || (flow && !flow_word_unmapped))
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t distance = (candidate - get) & mask;
    const uint32_t ahead = (put - get) & mask;
    /* The exact prologue witness reads through candidate+0x28 inclusive. */
    if (!candidate || distance < 4u || distance > max_gap ||
        ahead < distance + 0x2Cu)
        return 0u;
    return candidate;
}

/* Validate the larger generated-VP payload captured immediately after a
 * complete SET_BEGIN_END(0) packet.  The producer places the program bytes
 * after the draw-end pair and resumes at the next independently recognizable
 * generated draw prologue.  Some program words alias unmapped JUMPs/CALLs;
 * they are data only when the complete producer boundary below agrees.
 *
 * This is deliberately distinct from the NOOP-owned inline-gap family.  A
 * genuine mapped flow target is never bypassed, and the caller must re-read
 * the end packet, raw word, candidate prologue, balanced prefix and PUT after
 * a barrier before applying the result. */
static inline uint32_t yz_fifo_generated_vp_post_draw_candidate_resume(
    uint32_t previous_get, uint32_t previous_command,
    uint32_t previous_argument, uint32_t gap_word,
    uint32_t get, uint32_t put, uint32_t candidate,
    uint32_t fifo_size, uint32_t max_gap,
    int flow_word_unmapped,
    int generated_prologue_ready, int balanced_prefix_ready)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        (previous_get & 3u) || previous_get >= fifo_size ||
        (get & 3u) || get >= fifo_size || put >= fifo_size ||
        (candidate & 3u) || candidate >= fifo_size ||
        !max_gap || max_gap >= (fifo_size >> 1) ||
        previous_command != 0x00041808u || previous_argument != 0u ||
        ((previous_get + 8u) & (fifo_size - 1u)) != get ||
        generated_prologue_ready == 0 || balanced_prefix_ready == 0)
        return 0u;

    const int flow =
        ((gap_word & 0xE0000003u) == 0x20000000u) ||
        ((gap_word & 3u) == 1u) || ((gap_word & 3u) == 2u) ||
        ((gap_word & 0xFFFF0003u) == 0x00020000u);
    if (!gap_word || (flow && !flow_word_unmapped))
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t distance = (candidate - get) & mask;
    const uint32_t ahead = (put - get) & mask;
    if (!candidate || distance < 4u || distance > max_gap ||
        ahead < distance + 0x2Cu)
        return 0u;
    return candidate;
}

/* Retain the original fixed-shape helper as a source-compatible specialization
 * for its standalone regressions and any older callers. */
static inline uint32_t yz_fifo_generated_vp_inline_gap_resume(
    uint32_t previous_get, uint32_t previous_command,
    uint32_t gap_word, uint32_t get, uint32_t put, uint32_t fifo_size,
    int generated_prologue_ready, int balanced_prefix_ready)
{
    const uint32_t candidate = fifo_size
        ? (get + 0x28u) & (fifo_size - 1u) : 0u;
    return yz_fifo_generated_vp_inline_candidate_resume(
        previous_get, previous_command, gap_word, get, put, candidate,
        fifo_size, 0x28u, 0, generated_prologue_ready,
        balanced_prefix_ready);
}

/* Validate the EDGE generated-list block-boundary publication shape.  The
 * title links the primary FIFO to the final word of a fixed-size generated
 * block; that word is producer-owned flow storage and can still contain
 * recycled vertex/constant payload when the source JUMP becomes visible.
 * Execution may move to a candidate inside the following block only after the
 * caller has proven both its exact generated prologue and a complete
 * draw-balanced prefix.  A producer can leave recycled payload between the
 * block boundary and the first finalized prologue.
 *
 * This helper intentionally accepts packet-shaped recycled words: that is the
 * failure this boundary proof exists to distinguish.  A real flow word is
 * never bypassed, and no arbitrary scan result can satisfy the contract. */
static inline uint32_t yz_fifo_generated_block_candidate_resume(
    uint32_t tail, uint32_t tail_word, uint32_t candidate,
    uint32_t fifo_size, uint32_t block_size,
    int generated_prologue_ready, int balanced_prefix_ready)
{
    if (!fifo_size || (fifo_size & (fifo_size - 1u)) != 0u ||
        !block_size || (block_size & (block_size - 1u)) != 0u ||
        block_size > fifo_size || (tail & 3u) || tail >= fifo_size ||
        ((tail + 4u) & (block_size - 1u)) != 0u ||
        !candidate || (candidate & 3u) || candidate >= fifo_size ||
        generated_prologue_ready == 0 || balanced_prefix_ready == 0)
        return 0u;

    const int flow =
        ((tail_word & 0xE0000003u) == 0x20000000u) ||
        ((tail_word & 3u) == 1u) || ((tail_word & 3u) == 2u) ||
        ((tail_word & 0xFFFF0003u) == 0x00020000u);
    if (flow)
        return 0u;

    const uint32_t mask = fifo_size - 1u;
    const uint32_t block_start = (tail + 4u) & mask;
    const uint32_t candidate_delta = (candidate - block_start) & mask;
    return candidate_delta < block_size ? candidate : 0u;
}

static inline uint32_t yz_fifo_generated_block_tail_resume(
    uint32_t tail, uint32_t tail_word,
    uint32_t fifo_size, uint32_t block_size,
    int generated_prologue_ready, int balanced_prefix_ready)
{
    const uint32_t candidate = fifo_size
        ? (tail + 4u) & (fifo_size - 1u) : 0u;
    return yz_fifo_generated_block_candidate_resume(
        tail, tail_word, candidate, fifo_size, block_size,
        generated_prologue_ready, balanced_prefix_ready);
}

#endif
