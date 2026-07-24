#include "../rsx_dispatch.h"
#include <stdio.h>

int main(void)
{
    /* The sign's real case: a 16-bit source index still resolves above 64K. */
    if (rsx_dsp_resolve_vertex_index(5311, 65134) != 70445)
        return 1;

    /* The RSX addition itself wraps in a 20-bit element-index domain. */
    if (rsx_dsp_resolve_vertex_index(0x30, 0xFFFF0) != 0x20)
        return 1;

    printf("test_vertex_base_index: ALL PASS\n");
    return 0;
}
