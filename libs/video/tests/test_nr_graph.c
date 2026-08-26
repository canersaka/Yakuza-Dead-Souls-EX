#include "rsx_nr_graph.h"
#include "rsx_nir.h"

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; } } while (0)

int main(void)
{
    static const u32 dependency[] = {
        0x0050u, 0x0068u, 0x006Cu, 0x0110u, 0x17C8u, 0x1800u,
        0x1D70u, 0x1D74u, 0x2328u, 0xC40Cu, 0xE924u, 0xE944u,
        0xEB00u, 0xEB04u, 0xA400u, 0xAAFCu,
    };
    static const u32 new_pass[] = {
        0x018Cu, 0x0194u, 0x0198u, 0x01B4u, 0x01B8u,
        0x0200u, 0x0204u, 0x0208u, 0x020Cu, 0x0210u,
        0x0214u, 0x0218u, 0x021Cu, 0x0220u, 0x022Cu,
        0x0280u, 0x0284u, 0x0288u, 0x028Cu, 0x1D94u,
    };
    for (u32 i = 0; i < sizeof(dependency) / sizeof(dependency[0]); ++i)
        CHECK(rsx_nr_graph_classify_method(dependency[i]) ==
              RSX_NR_GRAPH_METHOD_DEPENDENCY);
    for (u32 i = 0; i < sizeof(new_pass) / sizeof(new_pass[0]); ++i)
        CHECK(rsx_nr_graph_classify_method(new_pass[i]) ==
              RSX_NR_GRAPH_METHOD_NEW_PASS);
    CHECK(rsx_nr_graph_classify_method(0x0300u) ==
          RSX_NR_GRAPH_METHOD_CONTINUE);
    CHECK(rsx_nr_graph_classify_method(0xA3FCu) ==
          RSX_NR_GRAPH_METHOD_CONTINUE);
    CHECK(rsx_nr_graph_classify_method(0xAB00u) ==
          RSX_NR_GRAPH_METHOD_CONTINUE);

    CHECK(!rsx_nr_graph_op_ends_island(RSX_NIR_OP_SET_VIEWPORT));
    CHECK(!rsx_nr_graph_op_ends_island(RSX_NIR_OP_DRAW));
    CHECK(!rsx_nr_graph_op_ends_island(RSX_NIR_OP_CLEAR));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_TRANSFER));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_PRESENT));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_SEMAPHORE_ACQUIRE));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_SEMAPHORE_RELEASE));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_REPORT));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_SET_REFERENCE));
    CHECK(rsx_nr_graph_op_ends_island(RSX_NIR_OP_USER_COMMAND));

    CHECK(rsx_nr_graph_can_enter(0, 0, 0, 0));
    CHECK(!rsx_nr_graph_can_enter(0, 1, 0, 0));
    CHECK(!rsx_nr_graph_can_enter(0, 0, 1, 0));
    CHECK(!rsx_nr_graph_can_enter(0, 0, 0, 1));
    CHECK(rsx_nr_graph_can_enter(1, 1, 1, 1));

    puts("rsx_nr_graph_tests: PASS");
    return 0;
}
