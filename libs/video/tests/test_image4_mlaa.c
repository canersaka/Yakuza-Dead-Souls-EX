#include "../rsx_image4_mlaa.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static void put16(unsigned char* p, unsigned value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static void put32(unsigned char* p, unsigned value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static void make_records(unsigned char r[5][64])
{
    memset(r, 0, 5u * 64u);
    for (unsigned id = 0; id < 5; ++id) {
        put32(r[id] + 0x0c, 0x41772d00u);
        put32(r[id] + 0x10, 0x41772d00u);
        put32(r[id] + 0x14, 0x42452700u);
        put32(r[id] + 0x18, 0x42452780u);
        put16(r[id] + 0x1c, 1024u);
        put16(r[id] + 0x1e, 768u);
        put16(r[id] + 0x20, 4096u);
        r[id][0x22] = 1;
        r[id][0x23] = (unsigned char)((id << 4) | 5u);
        put16(r[id] + 0x24, 10u);
        put16(r[id] + 0x26, 89u);
    }
    put32(r[0] + 0x00, 37u);
    put32(r[0] + 0x04, 0x10200fe0u);
    put32(r[0] + 0x08, 0x016222acu);
}

int main(void)
{
    unsigned char records[5][64];
    rsx_image4_mlaa_contract contract;
    make_records(records);
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_ACCEPT,
          "exact EDGE 1.2 five-task contract accepts");
    CHECK(contract.task[4].spu_id == 4u && contract.task[4].spu_count == 5u,
          "SPU id/count nibble decodes");
    CHECK(contract.counter_ea == 0x016222acu &&
              contract.task[0].threshold_base == 10u &&
              contract.task[0].threshold_scale == 89u,
          "publication fields retained");

    unsigned char reordered[5][64];
    for (unsigned i = 0; i < 5; ++i)
        memcpy(reordered[i], records[4u - i], 64u);
    CHECK(rsx_image4_mlaa_validate(reordered, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_ACCEPT,
          "record order is not semantic identity");

    make_records(records); records[3][0x22] = 3;
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_REJECT_MODE,
          "show-edges/unknown mode falls back");
    make_records(records); put16(records[2] + 0x26, 90u);
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_REJECT_THRESHOLD,
          "unknown threshold falls back");
    make_records(records); records[4][0x23] = 0x35;
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_REJECT_TASK_ID,
          "duplicate task identity falls back");
    make_records(records); put32(records[0] + 0x00, 38u);
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_REJECT_PUBLICATION,
          "wrong completion value falls back");
    make_records(records); records[1][0x3f] = 1;
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_REJECT_RESERVED,
          "unknown ABI extension falls back");

    rsx_image4_mlaa_image image = {
        RSX_IMAGE4_MLAA_FINGERPRINT, RSX_IMAGE4_MLAA_IMAGE_SIZE,
        RSX_IMAGE4_MLAA_ENTRY_PC, RSX_IMAGE4_MLAA_IMAGE_ID
    };
    CHECK(rsx_image4_mlaa_image_matches(&image),
          "exact SPU executable identity accepts");
    image.image_id = -1;
    CHECK(rsx_image4_mlaa_image_matches(&image),
          "host inventory number is not executable identity");
    image.entry_pc++;
    CHECK(!rsx_image4_mlaa_image_matches(&image),
          "changed executable identity rejects");

    make_records(records);
    CHECK(rsx_image4_mlaa_validate(records, 0x41772d00u,
              0x10200fe0u, 37u, &contract) == RSX_IMAGE4_MLAA_ACCEPT,
          "state-machine fixture validates");
    CHECK(rsx_image4_mlaa_counter_ready(&contract, 37u),
          "matching monotonic tasksReady epoch accepts");
    CHECK(!rsx_image4_mlaa_counter_ready(&contract, 36u) &&
          !rsx_image4_mlaa_counter_ready(&contract, 38u),
          "stale or completed tasksReady epoch rejects");
    rsx_image4_mlaa_round round;
    rsx_image4_mlaa_round_init(&round, 0);
    CHECK(rsx_image4_mlaa_round_offer(
              &round, 9u, &contract, 0u) == RSX_IMAGE4_MLAA_OFFER_REJECT,
          "disabled path is inert");
    rsx_image4_mlaa_round_init(&round, 1);
    for (unsigned id = 4; id > 0; --id)
        CHECK(rsx_image4_mlaa_round_offer(
                  &round, 9u, &contract, id) ==
                  RSX_IMAGE4_MLAA_OFFER_CONSUMED,
              "out-of-order exact signal consumed");
    CHECK(rsx_image4_mlaa_round_offer(
              &round, 9u, &contract, 4u) ==
              RSX_IMAGE4_MLAA_OFFER_DUPLICATE,
          "duplicate remains one round");
    CHECK(rsx_image4_mlaa_round_offer(
              &round, 9u, &contract, 0u) == RSX_IMAGE4_MLAA_OFFER_READY,
          "fifth unique signal makes one ready round");
    rsx_image4_mlaa_contract claimed;
    unsigned generation = 0;
    CHECK(!rsx_image4_mlaa_round_claim(
              &round, 0x10200fe4u, &claimed, &generation),
          "wrong acquire cannot claim");
    CHECK(rsx_image4_mlaa_round_claim(
              &round, 0x10200fe0u, &claimed, &generation) &&
              generation == 9u && claimed.label_value == 37u,
          "exact acquire claims once");
    CHECK(!rsx_image4_mlaa_round_claim(
              &round, 0x10200fe0u, &claimed, &generation),
          "claimed round cannot dispatch twice");
    CHECK(rsx_image4_mlaa_round_complete(&round) == 0 &&
              round.phase == RSX_IMAGE4_MLAA_PHASE_IDLE,
          "completion returns to idle");

    rsx_image4_mlaa_round_offer(&round, 10u, &contract, 0u);
    CHECK(!rsx_image4_mlaa_round_reject(&round, 11u) &&
              round.phase == RSX_IMAGE4_MLAA_PHASE_COLLECTING,
          "unrelated generation cannot poison admitted round");
    CHECK(rsx_image4_mlaa_round_reject(&round, 10u) &&
              round.phase == RSX_IMAGE4_MLAA_PHASE_FAULTED,
          "invalid later wake fails a partially admitted round closed");
    CHECK(rsx_image4_mlaa_round_reject(&round, 10u),
          "later wakes in the same faulted round remain consumed");
    rsx_image4_mlaa_round_reset(&round);
    CHECK(round.phase == RSX_IMAGE4_MLAA_PHASE_IDLE,
          "movie/reset handoff cancels an admitted round");
    for (unsigned id = 0; id < 5; ++id)
        rsx_image4_mlaa_round_offer(&round, 11u, &contract, id);
    CHECK(rsx_image4_mlaa_round_claim(
              &round, 0x10200fe0u, &claimed, &generation) &&
              generation == 11u,
          "a fresh round is accepted after movie/reset handoff");
    CHECK(rsx_image4_mlaa_round_complete(&round) == 0,
          "post-handoff round completes");

    rsx_image4_mlaa_round_offer(&round, 12u, &contract, 0u);
    rsx_image4_mlaa_contract changed = contract;
    changed.label_value++;
    CHECK(rsx_image4_mlaa_round_offer(
              &round, 12u, &changed, 1u) ==
              RSX_IMAGE4_MLAA_OFFER_CONFLICT &&
              round.phase == RSX_IMAGE4_MLAA_PHASE_FAULTED,
          "post-admission contract mutation fails closed");
    rsx_image4_mlaa_round_reset(&round);
    CHECK(round.phase == RSX_IMAGE4_MLAA_PHASE_IDLE &&
              round.signal_mask == 0u,
          "reset cancels a faulted or in-flight round");

    if (failures)
        return 1;
    puts("rsx_image4_mlaa: PASS");
    return 0;
}
