#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fixed-size, process-lifetime flight recorder for the post-movie completion
 * frontier.  The hot path is allocation-free and performs no formatting or
 * I/O.  YZ_FRONTIER_RING enables the dormant recorder; a semantic Job B
 * selection arms it.  YZ_FRONTIER_RING_PATH selects the dump-file prefix.
 */
enum yz_frontier_event_type {
    YZ_FT_ARM = 1,
    YZ_FT_PPU_SITE,
    YZ_FT_PPU_VALUE,
    YZ_FT_PPU_STORE,
    YZ_FT_SPU_JOB_SELECT,
    YZ_FT_SPU_DMA_CMD,
    YZ_FT_SPU_OUT_MBOX,
    YZ_FT_SPU_INTR_MBOX,
    YZ_FT_EVENT_WAIT,
    YZ_FT_EVENT_SET,
    YZ_FT_EVENT_QUEUE,
    YZ_FT_SPU_HALT,
    YZ_FT_RSX_STATE,
    YZ_FT_STALL,
    YZ_FT_FIFO_PARK,
    YZ_FT_FIFO_STATE,
    YZ_FT_FIFO_PUBLICATION,
    YZ_FT_SPU_STATE,
    YZ_FT_SPU_JOB_STATE,
    YZ_FT_SPU_MFC_STATE,
    YZ_FT_EVENT_STATE
};

enum yz_frontier_queue_phase {
    YZ_FT_QUEUE_RECEIVE_ENTER = 1,
    YZ_FT_QUEUE_RECEIVE_POP,
    YZ_FT_QUEUE_PUSH_FULL,
    YZ_FT_QUEUE_PUSH_OK
};

int yz_frontier_trace_init(void);
int yz_frontier_trace_enabled(void);
int yz_frontier_trace_is_armed(void);

void yz_frontier_trace_arm(uint32_t actor, uint32_t pc,
                           uint32_t source_ea, uint32_t ls,
                           uint32_t image, uint32_t descriptor);

void yz_frontier_trace_emit(uint32_t type, uint32_t actor, uint32_t pc,
                            uint32_t a0, uint32_t a1, uint32_t a2,
                            uint32_t a3, uint32_t a4, uint32_t a5);

/* One-shot dump. Returns 1 only for the caller that performed the dump. */
int yz_frontier_trace_dump(uint32_t reason);

/*
 * One-shot, read-only snapshots taken by the watchdog immediately before a
 * stall dump. They append compact records to the same ring; they never
 * format, allocate, or write files themselves.
 */
void yz_frontier_fifo_snapshot(uint32_t get, uint32_t put);
void yz_frontier_spu_snapshot(void);
void yz_frontier_event_snapshot(void);

#ifdef __cplusplus
}
#endif
