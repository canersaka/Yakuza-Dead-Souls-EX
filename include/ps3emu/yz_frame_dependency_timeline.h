#ifndef PS3EMU_YZ_FRAME_DEPENDENCY_TIMELINE_H
#define PS3EMU_YZ_FRAME_DEPENDENCY_TIMELINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default-off, fixed-memory, shutdown-only semantic dependency timeline. */
typedef enum yz_frame_dep_event_type {
    YZ_FRAME_DEP_PPU_UPDATE_START = 1,
    YZ_FRAME_DEP_PPU_UPDATE_COMPLETE,
    YZ_FRAME_DEP_SPURS_SCHEDULE,
    YZ_FRAME_DEP_SPU_TASK_START,
    YZ_FRAME_DEP_SPU_TASK_COMPLETE,
    YZ_FRAME_DEP_SPU_JOB_START,
    YZ_FRAME_DEP_SPU_JOB_COMPLETE,
    YZ_FRAME_DEP_DMA_PUBLISH,
    YZ_FRAME_DEP_PPU_WAIT_ENTER,
    YZ_FRAME_DEP_PPU_WAIT_EXIT,
    YZ_FRAME_DEP_FIFO_PUBLISH,
    YZ_FRAME_DEP_RSX_CONSUME,
    YZ_FRAME_DEP_FRAME_COMPLETE,
    YZ_FRAME_DEP_SUBMISSION,
    YZ_FRAME_DEP_PRESENT
} yz_frame_dep_event_type;

typedef struct yz_frame_dep_record {
    uint64_t sequence;
    uint64_t qpc;
    uint64_t frame_generation;
    uint64_t dependency_generation;
    uint32_t type;
    uint32_t thread_id;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
} yz_frame_dep_record;

extern volatile long g_yz_frame_dependency_timeline_enabled;

int yz_frame_dependency_timeline_init(void);
void yz_frame_dependency_timeline_shutdown(void);

uint64_t yz_frame_dep_ppu_update_start(uint32_t function_address,
                                        uint32_t object_ea);
void yz_frame_dep_ppu_update_complete(uint64_t frame_generation,
                                      uint32_t function_address,
                                      uint32_t result);
void yz_frame_dep_spurs_schedule(uint32_t kind, uint32_t image_id,
                                 uint32_t workload_id, uint32_t item_id);
void yz_frame_dep_spu_task_start(uint32_t image_id, uint32_t spu_id,
                                 uint32_t task_id, uint32_t pc);
void yz_frame_dep_spu_task_complete(uint32_t image_id, uint32_t spu_id,
                                    uint32_t task_id, uint32_t pc);
void yz_frame_dep_spu_job_start(uint32_t image_id, uint32_t spu_id,
                                uint32_t workload_id, uint32_t descriptor_ea);
void yz_frame_dep_spu_job_complete(uint32_t image_id, uint32_t spu_id,
                                   uint32_t workload_id, uint32_t descriptor_ea);

/* Returns zero when disabled or no registration slot is available. */
uint64_t yz_frame_dep_ppu_wait_enter(uint32_t address, uint32_t observed);
void yz_frame_dep_ppu_wait_exit(uint64_t token, uint32_t address,
                                uint32_t observed);
/* Called after DMA bytes and exact watched-address notification publish. */
void yz_frame_dep_dma_publish(uint32_t image_id, uint32_t spu_id,
                              uint32_t pc, uint32_t address,
                              uint32_t size, uint32_t command);
void yz_frame_dep_fifo_publish(uint32_t old_put, uint32_t new_put,
                               uint32_t source, uint32_t context_id);
void yz_frame_dep_rsx_consume(uint32_t old_get, uint32_t new_get,
                              uint32_t put, uint32_t result);
void yz_frame_dep_frame_complete(uint32_t buffer_id, uint64_t frame);
void yz_frame_dep_submission(uint32_t reason, uint64_t frame,
                             uint64_t fence);
void yz_frame_dep_present(uint32_t buffer_id, uint64_t frame,
                          uint32_t present_kind);

#if defined(YZ_FRAME_DEP_TIMELINE_TEST)
void yz_frame_dependency_test_reset(uint64_t frequency, int enabled);
void yz_frame_dependency_test_set_clock(uint64_t qpc);
uint64_t yz_frame_dependency_test_clock_reads(void);
uint64_t yz_frame_dependency_test_claimed(void);
int yz_frame_dependency_test_record(uint64_t sequence,
                                    yz_frame_dep_record* out);
#endif

#ifdef __cplusplus
}
#endif

#endif
