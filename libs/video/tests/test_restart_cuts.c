#include "../rsx_dispatch.h"
#include "../rsx_restart_cuts.h"
#include <stdio.h>

int main(void)
{
    u32* cuts = NULL;
    u32 count = 0, capacity = 0;
    for (u32 i = 0; i < 1539; i++) {
        if (!rsx_restart_cut_push(&cuts, &count, &capacity, (i + 1) * 4)) {
            free(cuts);
            return 1;
        }
    }
    if (count != 1539 || capacity < count || cuts[520] != 2084 ||
        cuts[1538] != 6156) {
        free(cuts);
        return 1;
    }
    u32 first = 0, span = 0;
    rsx_restart_segment_bounds(cuts, count, 6160, 1539, &first, &span);
    free(cuts);
    if (first != 6156 || span != 4)
        return 1;
    printf("test_restart_cuts: ALL PASS\n");
    return 0;
}
