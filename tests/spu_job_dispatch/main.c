#include "../../runtime/spu/spu_job_dispatch.h"

#include <stdio.h>

static int failures;
static int checks;

static void expect_image(
    const char* name, int current_image, uint32_t entry_pc,
    uint32_t binary_ea, int expected)
{
    checks++;
    const int actual =
        spu_job_descriptor_image(current_image, entry_pc, binary_ea);
    if (actual == expected)
        return;

    fprintf(stderr, "FAIL %s: expected image %d, got %d\n",
            name, expected, actual);
    failures++;
}

static void expect_resident_image(
    const char* name, int slot, uint32_t base, int expected)
{
    checks++;
    const int actual = spu_job_resident_image(slot, base);
    if (actual == expected)
        return;

    fprintf(stderr, "FAIL %s: expected image %d, got %d\n",
            name, expected, actual);
    failures++;
}

int main(void)
{
    /*
     * Regression: the orphanage worker may be dispatched before the a010
     * asset-open hook arms scene diagnostics.  Exact descriptor identity must
     * select image 19 without consulting that unrelated state.
     */
    expect_image("orphanage descriptor before scene activation",
                 13, 0x04C00u, 0x01252680u, 19);

    /*
     * Reverse handoff regression: after image 19 has occupied LS 0x4C00,
     * SPURS may omit the next Job A DMA.  Its descriptor must override the
     * stale residency tag so the runtime restores and executes image 14.
     */
    expect_image("job A descriptor after orphanage occupant",
                 13, 0x04C00u, 0x01254500u, 14);
    /*
     * Frontier regression: the Job D branch target is its actual 0x5000 load
     * base even when it overlaps an older 0x4C00 residency record.  Selecting
     * the 0x4C00 relocation shifts the source by 0x400 and destroys the
     * JOBCRT prologue that the Sony job module validates at target + 0x10.
     */
    expect_image("job D exact 5000 frontier slot",
                 13, 0x05000u, 0x01265180u, 26);
    expect_image("job D Kamurocho C400 slot",
                 13, 0x0C400u, 0x01265180u, 42);
    /*
     * A lifted call bracket can restore the preceding job image before the
     * module launches the next descriptor.  The launch descriptor remains
     * authoritative even when the current tag is image 19 rather than 13.
     */
    expect_image("job D descriptor after orphanage occupant",
                 19, 0x0E400u, 0x01265180u, 18);
    expect_image("job D alternate post-A030 slot",
                 13, 0x04C00u, 0x01265180u, 21);
    expect_image("job D high non-overlapping post-A030 slot",
                 14, 0x20C00u, 0x01265180u, 18);
    expect_image("job D second high post-A030 slot",
                 14, 0x23000u, 0x01265180u, 21);
    expect_image("job D overlapping 15800 post-A030 slot",
                 18, 0x15800u, 0x01265180u, 25);
    expect_image("job D live 27400 frontier slot",
                 18, 0x27400u, 0x01265180u, 32);
    expect_image("job D live 2A800 frontier slot",
                 18, 0x2A800u, 0x01265180u, 33);
    expect_image("job D live 2E400 frontier slot",
                 18, 0x2E400u, 0x01265180u, 34);
    expect_image("job D live 2C400 frontier slot",
                 18, 0x2C400u, 0x01265180u, 36);
    expect_image("job D live 2B400 frontier slot",
                 18, 0x2B400u, 0x01265180u, 29);
    expect_image("job D live 2D400 frontier slot",
                 18, 0x2D400u, 0x01265180u, 31);
    expect_image("job D live 2E800 frontier slot",
                 18, 0x2E800u, 0x01265180u, 30);
    expect_image("orphanage descriptor in alternate E400 slot",
                 19, 0x0E400u, 0x01252680u, 19);
    expect_image("orphanage descriptor in post-A030 3B000 slot",
                 19, 0x3B000u, 0x01252680u, 19);
    expect_image("orphanage descriptor in Kamurocho 37C00 slot",
                 35, 0x37C00u, 0x01252680u, 19);
    expect_image("orphanage descriptor in Kamurocho 2D800 slot",
                 19, 0x2D800u, 0x01252680u, 19);
    expect_image("job B alternate E400 slot after Job A",
                 14, 0x0E400u, 0x01275A00u, 15);
    expect_image("job B overlapping post-A030 slot",
                 13, 0x15400u, 0x01275A00u, 24);
    expect_image("job B high post-A030 slot",
                 13, 0x1EC00u, 0x01275A00u, 15);
    expect_image("job B live 3BC00 frontier slot",
                 29, 0x3BC00u, 0x01275A00u, 15);
    expect_image("job B live 3DC00 frontier slot",
                 15, 0x3DC00u, 0x01275A00u, 15);
    expect_image("job B post-dialogue 39C00 frontier slot",
                 15, 0x39C00u, 0x01275A00u, 15);
    expect_image("job B live 37C00 frontier slot",
                 15, 0x37C00u, 0x01275A00u, 35);
    expect_image("job B live 3B000 frontier slot",
                 15, 0x3B000u, 0x01275A00u, 37);
    expect_image("job B dialogue completion 3D000 slot",
                 15, 0x3D000u, 0x01275A00u, 43);
    expect_image("job A alternate E400 slot after Job B",
                 15, 0x0E400u, 0x01254500u, 14);
    expect_image("job A post-Kamurocho C400 slot",
                 38, 0x0C400u, 0x01254500u, 39);
    expect_image("job A Kamurocho 24400 slot",
                 14, 0x24400u, 0x01254500u, 14);
    expect_image("job A overlapping Kamurocho 2A400 slot",
                 14, 0x2A400u, 0x01254500u, 41);
    expect_image("job C fixed slot after Job D",
                 18, 0x17800u, 0x0125DA80u, 17);
    expect_image("job C Kamurocho E400 slot",
                 17, 0x0E400u, 0x0125DA80u, 17);
    expect_image("job C post-Kamurocho 15800 slot",
                 17, 0x15800u, 0x0125DA80u, 40);
    expect_image("job C live 4C00 frontier slot",
                 17, 0x04C00u, 0x0125DA80u, 38);
    expect_image("job C alternate post-A030 slot",
                 13, 0x1A800u, 0x0125DA80u, 20);
    expect_image("job C frontier 1AC00 slot",
                 17, 0x1AC00u, 0x0125DA80u, 27);
    expect_image("job C overlapping post-A030 slot",
                 13, 0x1B800u, 0x0125DA80u, 23);
    expect_image("job C second alternate post-A030 slot",
                 13, 0x1D800u, 0x0125DA80u, 22);
    expect_image("job C live-observed 1E800 slot",
                 24, 0x1E800u, 0x0125DA80u, 28);
    expect_image("unknown binary at shared entry",
                 13, 0x04C00u, 0x01250000u, -1);
    expect_image("descriptor outside job module",
                 0, 0x04C00u, 0x01252680u, -1);
    expect_image("descriptor before entry branch",
                 13, 0x04BFCu, 0x01252680u, -1);

    /*
     * Internal-call regression: after the exact descriptor selects Job D at
     * LS 0x5000, the residency-only path must retain image 26.  Falling back
     * to image 18 executes the E400 relocation and corrupts the caller's
     * saved r80/r81 before the post-Kamurocho loading frontier.
     */
    expect_resident_image("job C internal call at 4C00",
                          2, 0x04C00u, 38);
    expect_resident_image("job C internal call at 15800",
                          2, 0x15800u, 40);
    expect_resident_image("orphanage internal call at 37C00",
                          4, 0x37C00u, 19);
    expect_resident_image("orphanage internal call at 2D800",
                          4, 0x2D800u, 19);
    expect_resident_image("job A internal call at C400",
                          0, 0x0C400u, 39);
    expect_resident_image("job A internal call at 24400",
                          0, 0x24400u, 14);
    expect_resident_image("job A internal call at 2A400",
                          0, 0x2A400u, 41);
    expect_resident_image("job C internal call at E400",
                          2, 0x0E400u, 17);
    expect_resident_image("job D internal call at 5000",
                          3, 0x05000u, 26);
    expect_resident_image("job D internal call at E400",
                          3, 0x0E400u, 18);
    expect_resident_image("job D internal call at C400",
                          3, 0x0C400u, 42);
    expect_resident_image("job D internal call at 20C00",
                          3, 0x20C00u, 18);
    expect_resident_image("job D internal call at 4C00",
                          3, 0x04C00u, 21);
    expect_resident_image("job D internal call at 23000",
                          3, 0x23000u, 21);
    expect_resident_image("job D internal call at 15800",
                          3, 0x15800u, 25);
    expect_resident_image("job D internal call at 27400",
                          3, 0x27400u, 32);
    expect_resident_image("job D internal call at 2A800",
                          3, 0x2A800u, 33);
    expect_resident_image("job D internal call at 2E400",
                          3, 0x2E400u, 34);
    expect_resident_image("job D internal call at 2C400",
                          3, 0x2C400u, 36);
    expect_resident_image("job D internal call at 2B400",
                          3, 0x2B400u, 29);
    expect_resident_image("job D internal call at 2D400",
                          3, 0x2D400u, 31);
    expect_resident_image("job D internal call at 2E800",
                          3, 0x2E800u, 30);
    expect_resident_image("job B internal call at 3BC00",
                          1, 0x3BC00u, 15);
    expect_resident_image("job B internal call at 3DC00",
                          1, 0x3DC00u, 15);
    expect_resident_image("job B internal call at 37C00",
                          1, 0x37C00u, 35);
    expect_resident_image("job B internal call at 3B000",
                          1, 0x3B000u, 37);
    expect_resident_image("job B internal call at 3D000",
                          1, 0x3D000u, 43);
    expect_resident_image("job B internal call at 39C00",
                          1, 0x39C00u, 15);
    expect_resident_image("orphanage internal call at 3B000",
                          4, 0x3B000u, 19);

    if (failures)
        return 1;

    printf("spu_job_dispatch: %d checks passed\n", checks);
    return 0;
}
