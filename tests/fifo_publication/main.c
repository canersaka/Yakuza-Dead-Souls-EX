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

    /* Live hard-freeze witness: GET 0x34F180 was a producer-owned self-stop;
     * func_00EAB3DC published the immutable post-island cursor 0x4074FAA0. */
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

    if (!failed)
        puts("fifo publication island-edge regression: PASS");
    return failed ? 1 : 0;
}
