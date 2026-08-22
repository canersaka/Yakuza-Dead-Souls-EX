#ifndef PS3EMU_YZ_FE0_TIMELINE_H
#define PS3EMU_YZ_FE0_TIMELINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Focused, production-safe timeline for the image-4 -> 0x10200FE0 dependency.
 * It is disabled unless YZ_FE0_TIMELINE=1.  Enabled recording uses a fixed
 * ring and timestamps semantic boundaries only; shutdown is the only I/O.
 */
typedef enum yz_fe0_event_type {
    YZ_FE0_EVENT_UCMD_DISPATCH = 1,
    YZ_FE0_EVENT_CALLBACK_BEGIN,
    YZ_FE0_EVENT_CALLBACK_END,
    YZ_FE0_EVENT_WKL4_SIGNAL,
    YZ_FE0_EVENT_WKL4_WAKE,
    YZ_FE0_EVENT_WKL4_RESUME,
    YZ_FE0_EVENT_WKL4_HANDOFF,
    YZ_FE0_EVENT_WKL4_RECORD,
    YZ_FE0_EVENT_DMA_BEGIN,
    YZ_FE0_EVENT_PUBLISHED,
    YZ_FE0_EVENT_RSX_WAIT,
    YZ_FE0_EVENT_RSX_READY,
    /* Live ABI snapshot at an image-4 WAIT_SIGNAL handoff.  The fixed FE0
     * ring is already default-off and shutdown-only; retaining r81 here lets
     * us identify the SDK wait object's architectural lane without adding a
     * synchronous task-path logger. */
    YZ_FE0_EVENT_WKL4_WAIT_ABI,
    YZ_FE0_EVENT_WKL4_EVENT_OBSERVE,
    YZ_FE0_EVENT_WKL4_EVENT_WRITE,
    YZ_FE0_EVENT_WKL4_QUEUE_WAIT,
    YZ_FE0_EVENT_CALLBACK_STATE,
    /* Registered CellSpursLFQueue semantic boundaries.  INIT is emitted at
     * most once per queue.  WRITE is emitted only for a real first-16-byte
     * state transition after image 4 has named that queue at WAIT_SIGNAL;
     * PPU_PUSH is one record per successful HLE push. */
    YZ_FE0_EVENT_WKL4_QUEUE_INIT,
    YZ_FE0_EVENT_WKL4_QUEUE_OBSERVE,
    YZ_FE0_EVENT_WKL4_QUEUE_WRITE,
    YZ_FE0_EVENT_WKL4_QUEUE_PPU_PUSH,
    /* Eight 16-byte records sampled once per 256 image-4 queue waits.  The
     * actor is the queue-byte offset and a0..a3 are the four big-endian
     * words.  This locates SDK waiter/publication fields without routing or
     * logging every write to the complete 128-byte object. */
    YZ_FE0_EVENT_WKL4_QUEUE_LAYOUT,
    /* The r81 object observed at the image-4 WAIT_SIGNAL boundary is a
     * CellSpursBarrier (total at +0x04, taskset EA at +0x34), not a queue.
     * These records cover only successful lock-line publications at that
     * barrier and at its owning taskset signal field. */
    YZ_FE0_EVENT_WKL4_BARRIER_WAIT,
    YZ_FE0_EVENT_WKL4_BARRIER_LAYOUT,
    YZ_FE0_EVENT_WKL4_BARRIER_WRITE,
    YZ_FE0_EVENT_WKL4_TASKSET_WRITE
} yz_fe0_event_type;

typedef struct yz_fe0_timeline_record {
    uint64_t sequence;
    uint64_t qpc;
    /* Per-thread user+kernel time in 100 ns units. Populated only for the
     * image-4 RESUME/HANDOFF boundaries; zero for every other event. */
    uint64_t thread_time_100ns;
    /* Precise cycle counter from the same host thread and boundaries. */
    uint64_t thread_cycles;
    uint32_t type;
    uint32_t thread_id;
    uint32_t cause;
    uint32_t actor;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
} yz_fe0_timeline_record;

extern volatile long g_yz_fe0_timeline_enabled;

int yz_fe0_timeline_init(void);
void yz_fe0_timeline_shutdown(void);
void yz_fe0_timeline_emit(yz_fe0_event_type type, uint32_t cause,
                          uint32_t actor, uint32_t a0, uint32_t a1,
                          uint32_t a2, uint32_t a3);
void yz_fe0_timeline_callback_begin(uint32_t cause, uint32_t epoch);
void yz_fe0_timeline_callback_end(uint32_t cause, uint32_t epoch);
int yz_fe0_timeline_callback_snapshot(uint32_t* cause, uint32_t* epoch);

/* Deduplicated acquire boundary: repeated failed polls of one key do no work. */
void yz_fe0_timeline_rsx_acquire(uint32_t context_dma, uint32_t offset,
                                 uint32_t address, uint32_t wanted,
                                 uint32_t observed, int stalled);

/* Register the one structurally validated image-4 barrier and retain only
 * semantic successful PUTLLC publications to it or its taskset signal line.
 * The caller checks g_yz_fe0_timeline_enabled before entering the atomic
 * observer, so the disabled production path performs no clock reads. */
void yz_fe0_timeline_set_wkl4_barrier(uint32_t barrier_ea,
                                      uint32_t taskset_ea);
void yz_fe0_timeline_observe_wkl4_atomic(
    uint32_t image_id, uint32_t spu_id, uint32_t pc,
    uint32_t cause, uint32_t task, uint32_t line_ea,
    const uint8_t* before, const uint8_t* after);

/* Observe the barrier-release SendSignal lock-line transaction before its
 * PUTLLC verdict is applied.  This is intentionally separate from the
 * successful-publication observer above: a failed reservation or a
 * successful no-op must remain visible without adding per-attempt clocks or
 * output.  Only image 4's exact SendSignal PUTLLC site and the registered
 * workload-4 taskset line are retained. */
void yz_fe0_timeline_observe_wkl4_taskset_attempt(
    uint32_t image_id, uint32_t spu_id, uint32_t pc,
    uint32_t cause, uint32_t task, uint32_t line_ea,
    int success, const uint8_t* current, const uint8_t* candidate);

#if defined(YZ_FE0_TIMELINE_TEST)
void yz_fe0_timeline_test_reset(uint64_t frequency, int enabled);
void yz_fe0_timeline_test_set_clock(uint64_t qpc);
uint64_t yz_fe0_timeline_test_clock_reads(void);
void yz_fe0_timeline_test_set_thread_time(uint64_t time_100ns);
uint64_t yz_fe0_timeline_test_thread_time_reads(void);
void yz_fe0_timeline_test_set_thread_cycles(uint64_t cycles);
uint64_t yz_fe0_timeline_test_thread_cycle_reads(void);
uint64_t yz_fe0_timeline_test_claimed(void);
int yz_fe0_timeline_test_record(uint64_t sequence,
                                yz_fe0_timeline_record* out);
uint32_t yz_fe0_timeline_test_taskset_attempts(void);
uint32_t yz_fe0_timeline_test_taskset_successes(void);
uint32_t yz_fe0_timeline_test_taskset_changes(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
