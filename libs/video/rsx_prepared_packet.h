#ifndef RSX_PREPARED_PACKET_H
#define RSX_PREPARED_PACKET_H
#include "rsx_dispatch.h"
#include "rsx_commands.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rsx_prepared_packet_cache rsx_prepared_packet_cache;
typedef struct rsx_prepared_packet_stats { u64 packets, methods, hits, compiled, refused, actions; } rsx_prepared_packet_stats;
rsx_prepared_packet_cache* rsx_prepared_packet_create(void);
void rsx_prepared_packet_destroy(rsx_prepared_packet_cache* cache);
void rsx_prepared_packet_get_stats(const rsx_prepared_packet_cache* cache, rsx_prepared_packet_stats* out);
/* Compile the exact header's method schema; no argument values or guest
 * addresses are cached. Reject side effects outside the register model,
 * other subchannels, malformed headers and packets with interior actions. */
int rsx_prepared_packet_supported(rsx_prepared_packet_cache* cache, u32 header);
/* Sole graphics owner. The optional protocol state retains ONLY report DMA
 * selection; no auxiliary graphics backend is called or kept in sync. Caller
 * must ensure no other graphics backend is active while this owns the
 * register file. */
int rsx_prepared_packet_execute_graphics(rsx_prepared_packet_cache* cache,
    rsx_dispatch* renderer, rsx_state* protocol, u32 header,
    const u32* arguments, u64* method_counter);
#ifdef __cplusplus
}
#endif
#endif
