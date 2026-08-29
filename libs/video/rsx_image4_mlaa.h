#ifndef PS3RECOMP_RSX_IMAGE4_MLAA_H
#define PS3RECOMP_RSX_IMAGE4_MLAA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RSX_IMAGE4_MLAA_TASKS = 5,
    RSX_IMAGE4_MLAA_RECORD_BYTES = 64,
    RSX_IMAGE4_MLAA_WIDTH = 1024,
    RSX_IMAGE4_MLAA_HEIGHT = 768,
    RSX_IMAGE4_MLAA_PITCH = 4096,
    RSX_IMAGE4_MLAA_MODE_ENABLED = 1,
    RSX_IMAGE4_MLAA_THRESHOLD_BASE = 10,
    RSX_IMAGE4_MLAA_THRESHOLD_SCALE = 89,
    RSX_IMAGE4_MLAA_IMAGE_ID = 4,
    /* Native SPURS classifies the complete live ELF image: the 0x8420 code
     * payload plus its 0x34-byte ELF header. */
    RSX_IMAGE4_MLAA_IMAGE_SIZE = 0x8454,
    RSX_IMAGE4_MLAA_ENTRY_PC = 0x3050
};

#define RSX_IMAGE4_MLAA_FINGERPRINT UINT64_C(0xF6B85AD5F0FE9E78)

typedef struct rsx_image4_mlaa_task {
    uint32_t label_value;
    uint32_t label_ea;
    uint32_t counter_ea;
    uint32_t image_ea;
    uint32_t dest_ea;
    uint32_t barrier_ea;
    uint32_t direction_lock_ea;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint8_t mode;
    uint8_t spu_id;
    uint8_t spu_count;
    uint16_t threshold_base;
    uint16_t threshold_scale;
} rsx_image4_mlaa_task;

typedef struct rsx_image4_mlaa_contract {
    rsx_image4_mlaa_task task[RSX_IMAGE4_MLAA_TASKS];
    uint32_t image_ea;
    uint32_t label_ea;
    uint32_t label_value;
    uint32_t counter_ea;
    uint32_t barrier_ea;
    uint32_t direction_lock_ea;
} rsx_image4_mlaa_contract;

typedef enum rsx_image4_mlaa_reject {
    RSX_IMAGE4_MLAA_ACCEPT = 0,
    RSX_IMAGE4_MLAA_REJECT_ARGUMENT,
    RSX_IMAGE4_MLAA_REJECT_RECORD,
    RSX_IMAGE4_MLAA_REJECT_TASK_ID,
    RSX_IMAGE4_MLAA_REJECT_TASK_COUNT,
    RSX_IMAGE4_MLAA_REJECT_SHAPE,
    RSX_IMAGE4_MLAA_REJECT_MODE,
    RSX_IMAGE4_MLAA_REJECT_THRESHOLD,
    RSX_IMAGE4_MLAA_REJECT_IMAGE,
    RSX_IMAGE4_MLAA_REJECT_SYNC,
    RSX_IMAGE4_MLAA_REJECT_PUBLICATION,
    RSX_IMAGE4_MLAA_REJECT_RESERVED
} rsx_image4_mlaa_reject;

typedef struct rsx_image4_mlaa_image {
    uint64_t fingerprint;
    uint32_t image_size;
    uint32_t entry_pc;
    int32_t image_id;
} rsx_image4_mlaa_image;

typedef enum rsx_image4_mlaa_phase {
    RSX_IMAGE4_MLAA_PHASE_DISABLED = 0,
    RSX_IMAGE4_MLAA_PHASE_IDLE,
    RSX_IMAGE4_MLAA_PHASE_COLLECTING,
    RSX_IMAGE4_MLAA_PHASE_READY,
    RSX_IMAGE4_MLAA_PHASE_CLAIMED,
    RSX_IMAGE4_MLAA_PHASE_FAULTED
} rsx_image4_mlaa_phase;

typedef struct rsx_image4_mlaa_round {
    uint32_t phase;
    uint32_t generation;
    uint32_t signal_mask;
    rsx_image4_mlaa_contract contract;
} rsx_image4_mlaa_round;

typedef enum rsx_image4_mlaa_offer_result {
    RSX_IMAGE4_MLAA_OFFER_REJECT = 0,
    RSX_IMAGE4_MLAA_OFFER_CONSUMED,
    RSX_IMAGE4_MLAA_OFFER_DUPLICATE,
    RSX_IMAGE4_MLAA_OFFER_READY,
    RSX_IMAGE4_MLAA_OFFER_CONFLICT
} rsx_image4_mlaa_offer_result;

/* Parse the big-endian EDGE 1.2 EdgePostMlaaTaskParameters ABI. */
int rsx_image4_mlaa_parse_task(
    const uint8_t record[RSX_IMAGE4_MLAA_RECORD_BYTES],
    rsx_image4_mlaa_task* out);

/* Admit only the exact five-task game route. Records may arrive in any task
 * order; spu_id is the semantic identity. Unknown EDGE modes and variants
 * deliberately reject so the persistent SPURS tasks remain authoritative. */
rsx_image4_mlaa_reject rsx_image4_mlaa_validate(
    const uint8_t records[RSX_IMAGE4_MLAA_TASKS]
                         [RSX_IMAGE4_MLAA_RECORD_BYTES],
    uint32_t expected_image_ea, uint32_t expected_label_ea,
    uint32_t expected_label_value, rsx_image4_mlaa_contract* out);

const char* rsx_image4_mlaa_reject_name(rsx_image4_mlaa_reject reject);

/* Exact lifted-image identity. This is deliberately independent of image_id:
 * the registry id is useful routing metadata, while fingerprint+size+entry
 * are the fail-closed executable identity. */
int rsx_image4_mlaa_image_matches(const rsx_image4_mlaa_image* image);
/* This title uses a monotonic tasksReady epoch: before a round, the counter
 * equals that round's label value; task 0 increments it after publishing the
 * completed output. */
int rsx_image4_mlaa_counter_ready(
    const rsx_image4_mlaa_contract* contract, uint32_t counter_value);

/* Allocation-free admission state machine. Callers provide synchronization.
 * Once the first exact signal is consumed, an inconsistent later signal is a
 * conflict rather than a partial fallback: some SPURS work has already been
 * replaced and mixing execution would violate the task barrier contract. */
void rsx_image4_mlaa_round_init(rsx_image4_mlaa_round* round, int enabled);
void rsx_image4_mlaa_round_reset(rsx_image4_mlaa_round* round);
rsx_image4_mlaa_offer_result rsx_image4_mlaa_round_offer(
    rsx_image4_mlaa_round* round, uint32_t generation,
    const rsx_image4_mlaa_contract* contract, uint32_t spu_id);
/* Return nonzero when an invalid later wake belongs to a round whose first
 * wake was already consumed. Such a round must fail closed; falling only the
 * later task back to SPURS would strand the five-task EDGE barrier. */
int rsx_image4_mlaa_round_reject(
    rsx_image4_mlaa_round* round, uint32_t generation);
int rsx_image4_mlaa_round_claim(
    rsx_image4_mlaa_round* round, uint32_t label_ea,
    rsx_image4_mlaa_contract* contract, uint32_t* generation);
int rsx_image4_mlaa_round_complete(rsx_image4_mlaa_round* round);

#ifdef __cplusplus
}
#endif
#endif
