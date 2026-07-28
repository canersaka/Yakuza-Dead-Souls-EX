#include "../../runtime/spu/spu_job_dispatch.h"

#include <stdio.h>

static int failures;

static void expect_image(
    const char* name, int current_image, uint32_t entry_pc,
    uint32_t binary_ea, int expected)
{
    const int actual =
        spu_job_descriptor_image(current_image, entry_pc, binary_ea);
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

    expect_image("different binary at shared entry",
                 13, 0x04C00u, 0x01254500u, -1);
    expect_image("descriptor outside job module",
                 0, 0x04C00u, 0x01252680u, -1);
    expect_image("descriptor before entry branch",
                 13, 0x04BFCu, 0x01252680u, -1);

    if (failures)
        return 1;

    puts("spu_job_dispatch: 4 checks passed");
    return 0;
}
