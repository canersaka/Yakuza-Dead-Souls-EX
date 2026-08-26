#include "rsx_nr_graph.h"
#include "rsx_nir.h"

static int surface_method(u32 method)
{
    switch (method) {
    case 0x018Cu: case 0x0194u: case 0x0198u:
    case 0x01B4u: case 0x01B8u:
    case 0x0200u: case 0x0204u: case 0x0208u:
    case 0x020Cu: case 0x0210u: case 0x0214u:
    case 0x0218u: case 0x021Cu: case 0x0220u:
    case 0x022Cu: case 0x0280u: case 0x0284u:
    case 0x0288u: case 0x028Cu:
        return 1;
    default:
        return 0;
    }
}

rsx_nr_graph_method_boundary rsx_nr_graph_classify_method(u32 method)
{
    if (method == 0x1D94u || surface_method(method))
        return RSX_NR_GRAPH_METHOD_NEW_PASS;
    switch (method) {
    case 0x0050u: case 0x0068u: case 0x006Cu:
    case 0x0110u: case 0x17C8u: case 0x1800u:
    case 0x1D70u: case 0x1D74u: case 0x2328u:
    case 0xC40Cu: case 0xE924u: case 0xE944u:
    case 0xEB00u: case 0xEB04u:
        return RSX_NR_GRAPH_METHOD_DEPENDENCY;
    default:
        return method >= 0xA400u && method <= 0xAAFCu
            ? RSX_NR_GRAPH_METHOD_DEPENDENCY
            : RSX_NR_GRAPH_METHOD_CONTINUE;
    }
}

int rsx_nr_graph_op_ends_island(u32 kind)
{
    /* A draw/clear is the first consumer of the guest resources accumulated
     * by the island. Execute that fully recorded island before publishing the
     * action packet's GET, so a producer cannot overwrite vertex, texture, or
     * target data between preflight and consumption. This is an execution
     * boundary only; it does not submit the shared D3D command list. */
    return kind == RSX_NIR_OP_DRAW ||
           kind == RSX_NIR_OP_CLEAR ||
           kind == RSX_NIR_OP_TRANSFER ||
           kind == RSX_NIR_OP_PRESENT ||
           kind == RSX_NIR_OP_SEMAPHORE_ACQUIRE ||
           kind == RSX_NIR_OP_SEMAPHORE_RELEASE ||
           kind == RSX_NIR_OP_REPORT ||
           kind == RSX_NIR_OP_BARRIER ||
           kind == RSX_NIR_OP_SET_REFERENCE ||
           kind == RSX_NIR_OP_USER_COMMAND ||
           kind == RSX_NIR_OP_TOKEN_WAIT ||
           kind == RSX_NIR_OP_TOKEN_SIGNAL;
}

int rsx_nr_graph_can_enter(int section_pending, int packet_active,
                           int method_inflight, u32 ring_depth)
{
    return section_pending ||
           (!packet_active && !method_inflight && ring_depth == 0u);
}
