/* Fixed-policy boundaries for strict native dependency islands. */
#ifndef PS3RECOMP_RSX_NR_GRAPH_H
#define PS3RECOMP_RSX_NR_GRAPH_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rsx_nr_graph_method_boundary {
    RSX_NR_GRAPH_METHOD_CONTINUE = 0,
    RSX_NR_GRAPH_METHOD_DEPENDENCY,
    RSX_NR_GRAPH_METHOD_NEW_PASS,
} rsx_nr_graph_method_boundary;

/* Called before decoding a FIFO packet.  A non-CONTINUE result closes the
 * preceding island without consuming any method from the next island. */
rsx_nr_graph_method_boundary rsx_nr_graph_classify_method(u32 method);

/* Called after typed adaptation. These guest-visible actions terminate the
 * current island after the complete packet containing the action. */
int rsx_nr_graph_op_ends_island(u32 kind);

/* Resource-snapshot execution owns a complete render-pass slice rather than
 * ending at each draw. Guest-visible dependencies remain exact boundaries;
 * draw and clear work can share one withheld, atomically prepared island. */
int rsx_nr_graph_op_ends_snapshot_island(u32 kind);

/* A graph scan must never interleave with an immediate owner packet retained
 * across calls (most importantly a blocked semaphore acquire). */
int rsx_nr_graph_can_enter(int section_pending, int packet_active,
                           int method_inflight, u32 ring_depth);

#ifdef __cplusplus
}
#endif
#endif
