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

#endif
