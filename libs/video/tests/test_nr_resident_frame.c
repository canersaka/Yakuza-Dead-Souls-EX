#include "rsx_nr_resident_frame.h"

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; \
} } while (0)

int main(void)
{
    rsx_nr_resident_frame state;
    rsx_nr_resident_frame_init(&state, 0);
    CHECK(rsx_nr_resident_frame_submit(&state, 7u) == 7u);
    CHECK(state.allocator_slot == 0u);

    rsx_nr_resident_frame_init(&state, 1);
    CHECK(rsx_nr_resident_frame_submit(&state, 1u) == 0u);
    CHECK(rsx_nr_resident_frame_submit(&state, 2u) == 0u);
    CHECK(rsx_nr_resident_frame_submit(&state, 3u) == 0u);
    CHECK(rsx_nr_resident_frame_submit(&state, 4u) == 1u);
    CHECK(rsx_nr_resident_frame_submit(&state, 5u) == 2u);

    CHECK(!rsx_nr_resident_frame_can_recycle(5u, 4u));
    CHECK(rsx_nr_resident_frame_can_recycle(5u, 5u));
    CHECK(rsx_nr_resident_frame_can_recycle(5u, 9u));
    CHECK(!rsx_nr_resident_frame_can_recycle(0u, UINT64_MAX));

    puts("resident-frame lifetime tests passed");
    return 0;
}
