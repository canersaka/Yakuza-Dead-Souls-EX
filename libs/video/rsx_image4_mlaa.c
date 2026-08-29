#include "rsx_image4_mlaa.h"

#include <string.h>

static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int rsx_image4_mlaa_parse_task(
    const uint8_t record[RSX_IMAGE4_MLAA_RECORD_BYTES],
    rsx_image4_mlaa_task* out)
{
    if (!record || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->label_value = be32(record + 0x00);
    out->label_ea = be32(record + 0x04);
    out->counter_ea = be32(record + 0x08);
    out->image_ea = be32(record + 0x0c);
    out->dest_ea = be32(record + 0x10);
    out->barrier_ea = be32(record + 0x14);
    out->direction_lock_ea = be32(record + 0x18);
    out->width = be16(record + 0x1c);
    out->height = be16(record + 0x1e);
    out->pitch = be16(record + 0x20);
    out->mode = record[0x22];
    out->spu_id = record[0x23] >> 4;
    out->spu_count = record[0x23] & 0x0f;
    out->threshold_base = be16(record + 0x24);
    out->threshold_scale = be16(record + 0x26);
    return 0;
}

rsx_image4_mlaa_reject rsx_image4_mlaa_validate(
    const uint8_t records[RSX_IMAGE4_MLAA_TASKS]
                         [RSX_IMAGE4_MLAA_RECORD_BYTES],
    uint32_t expected_image_ea, uint32_t expected_label_ea,
    uint32_t expected_label_value, rsx_image4_mlaa_contract* out)
{
    rsx_image4_mlaa_contract result;
    uint32_t seen = 0;
    if (!records || !out || !expected_image_ea || !expected_label_ea)
        return RSX_IMAGE4_MLAA_REJECT_ARGUMENT;
    memset(&result, 0, sizeof(result));

    for (uint32_t input = 0; input < RSX_IMAGE4_MLAA_TASKS; ++input) {
        rsx_image4_mlaa_task task;
        if (rsx_image4_mlaa_parse_task(records[input], &task) != 0)
            return RSX_IMAGE4_MLAA_REJECT_RECORD;
        if (task.spu_id >= RSX_IMAGE4_MLAA_TASKS ||
            (seen & (1u << task.spu_id)))
            return RSX_IMAGE4_MLAA_REJECT_TASK_ID;
        seen |= 1u << task.spu_id;
        if (task.spu_count != RSX_IMAGE4_MLAA_TASKS)
            return RSX_IMAGE4_MLAA_REJECT_TASK_COUNT;
        if (task.width != RSX_IMAGE4_MLAA_WIDTH ||
            task.height != RSX_IMAGE4_MLAA_HEIGHT ||
            task.pitch != RSX_IMAGE4_MLAA_PITCH)
            return RSX_IMAGE4_MLAA_REJECT_SHAPE;
        if (task.mode != RSX_IMAGE4_MLAA_MODE_ENABLED)
            return RSX_IMAGE4_MLAA_REJECT_MODE;
        if (task.threshold_base != RSX_IMAGE4_MLAA_THRESHOLD_BASE ||
            task.threshold_scale != RSX_IMAGE4_MLAA_THRESHOLD_SCALE)
            return RSX_IMAGE4_MLAA_REJECT_THRESHOLD;
        if (task.image_ea != expected_image_ea ||
            task.dest_ea != expected_image_ea)
            return RSX_IMAGE4_MLAA_REJECT_IMAGE;
        if (!task.barrier_ea || (task.barrier_ea & 127u) ||
            !task.direction_lock_ea || (task.direction_lock_ea & 127u))
            return RSX_IMAGE4_MLAA_REJECT_SYNC;
        for (uint32_t i = 0x28; i < RSX_IMAGE4_MLAA_RECORD_BYTES; ++i)
            if (records[input][i] != 0)
                return RSX_IMAGE4_MLAA_REJECT_RESERVED;
        result.task[task.spu_id] = task;
    }
    if (seen != (1u << RSX_IMAGE4_MLAA_TASKS) - 1u)
        return RSX_IMAGE4_MLAA_REJECT_TASK_ID;

    const rsx_image4_mlaa_task* first = &result.task[0];
    if (first->label_ea != expected_label_ea ||
        first->label_value != expected_label_value ||
        !first->counter_ea)
        return RSX_IMAGE4_MLAA_REJECT_PUBLICATION;
    for (uint32_t id = 1; id < RSX_IMAGE4_MLAA_TASKS; ++id) {
        const rsx_image4_mlaa_task* task = &result.task[id];
        if (task->label_value || task->label_ea || task->counter_ea)
            return RSX_IMAGE4_MLAA_REJECT_PUBLICATION;
        if (task->barrier_ea != first->barrier_ea ||
            task->direction_lock_ea != first->direction_lock_ea)
            return RSX_IMAGE4_MLAA_REJECT_SYNC;
    }
    result.image_ea = expected_image_ea;
    result.label_ea = first->label_ea;
    result.label_value = first->label_value;
    result.counter_ea = first->counter_ea;
    result.barrier_ea = first->barrier_ea;
    result.direction_lock_ea = first->direction_lock_ea;
    *out = result;
    return RSX_IMAGE4_MLAA_ACCEPT;
}

const char* rsx_image4_mlaa_reject_name(rsx_image4_mlaa_reject reject)
{
    static const char* const names[] = {
        "accept", "argument", "record", "task-id", "task-count",
        "shape", "mode", "threshold", "image", "sync",
        "publication", "reserved"
    };
    return (unsigned)reject < sizeof(names) / sizeof(names[0])
        ? names[reject] : "unknown";
}

int rsx_image4_mlaa_image_matches(const rsx_image4_mlaa_image* image)
{
    return image &&
        image->fingerprint == RSX_IMAGE4_MLAA_FINGERPRINT &&
        image->image_size == RSX_IMAGE4_MLAA_IMAGE_SIZE &&
        image->entry_pc == RSX_IMAGE4_MLAA_ENTRY_PC;
}

int rsx_image4_mlaa_counter_ready(
    const rsx_image4_mlaa_contract* contract, uint32_t counter_value)
{
    return contract && contract->counter_ea &&
        counter_value == contract->label_value;
}

void rsx_image4_mlaa_round_init(rsx_image4_mlaa_round* round, int enabled)
{
    if (!round)
        return;
    memset(round, 0, sizeof(*round));
    round->phase = enabled ? RSX_IMAGE4_MLAA_PHASE_IDLE
                           : RSX_IMAGE4_MLAA_PHASE_DISABLED;
}

void rsx_image4_mlaa_round_reset(rsx_image4_mlaa_round* round)
{
    if (!round || round->phase == RSX_IMAGE4_MLAA_PHASE_DISABLED)
        return;
    memset(round, 0, sizeof(*round));
    round->phase = RSX_IMAGE4_MLAA_PHASE_IDLE;
}

rsx_image4_mlaa_offer_result rsx_image4_mlaa_round_offer(
    rsx_image4_mlaa_round* round, uint32_t generation,
    const rsx_image4_mlaa_contract* contract, uint32_t spu_id)
{
    const uint32_t all = (1u << RSX_IMAGE4_MLAA_TASKS) - 1u;
    if (!round || !contract || !generation ||
        spu_id >= RSX_IMAGE4_MLAA_TASKS ||
        round->phase == RSX_IMAGE4_MLAA_PHASE_DISABLED ||
        round->phase == RSX_IMAGE4_MLAA_PHASE_CLAIMED ||
        round->phase == RSX_IMAGE4_MLAA_PHASE_FAULTED)
        return RSX_IMAGE4_MLAA_OFFER_REJECT;

    if (round->phase == RSX_IMAGE4_MLAA_PHASE_IDLE) {
        round->phase = RSX_IMAGE4_MLAA_PHASE_COLLECTING;
        round->generation = generation;
        round->contract = *contract;
    } else if (round->generation != generation ||
               memcmp(&round->contract, contract, sizeof(*contract)) != 0) {
        round->phase = RSX_IMAGE4_MLAA_PHASE_FAULTED;
        return RSX_IMAGE4_MLAA_OFFER_CONFLICT;
    }

    const uint32_t bit = 1u << spu_id;
    if (round->signal_mask & bit)
        return RSX_IMAGE4_MLAA_OFFER_DUPLICATE;
    round->signal_mask |= bit;
    if (round->signal_mask == all) {
        round->phase = RSX_IMAGE4_MLAA_PHASE_READY;
        return RSX_IMAGE4_MLAA_OFFER_READY;
    }
    return RSX_IMAGE4_MLAA_OFFER_CONSUMED;
}

int rsx_image4_mlaa_round_reject(
    rsx_image4_mlaa_round* round, uint32_t generation)
{
    if (!round || !generation ||
        (round->phase != RSX_IMAGE4_MLAA_PHASE_COLLECTING &&
         round->phase != RSX_IMAGE4_MLAA_PHASE_READY &&
         round->phase != RSX_IMAGE4_MLAA_PHASE_FAULTED) ||
        round->generation != generation || !round->signal_mask)
        return 0;
    round->phase = RSX_IMAGE4_MLAA_PHASE_FAULTED;
    return 1;
}

int rsx_image4_mlaa_round_claim(
    rsx_image4_mlaa_round* round, uint32_t label_ea,
    rsx_image4_mlaa_contract* contract, uint32_t* generation)
{
    if (!round || !contract || !generation ||
        round->phase != RSX_IMAGE4_MLAA_PHASE_READY || !label_ea ||
        label_ea != round->contract.label_ea)
        return 0;
    round->phase = RSX_IMAGE4_MLAA_PHASE_CLAIMED;
    *contract = round->contract;
    *generation = round->generation;
    return 1;
}

int rsx_image4_mlaa_round_complete(rsx_image4_mlaa_round* round)
{
    if (!round || round->phase != RSX_IMAGE4_MLAA_PHASE_CLAIMED)
        return -1;
    rsx_image4_mlaa_round_reset(round);
    return 0;
}
