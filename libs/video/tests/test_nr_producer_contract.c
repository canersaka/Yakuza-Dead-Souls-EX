#include "rsx_nr_producer_contract.h"

#include <stdio.h>

static int failures;

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        failures++; \
    } \
} while (0)

int main(void)
{
    CHECK(rsx_nr_direct_setter_count() == 25u);
    for (u32 i = 0; i < rsx_nr_direct_setter_count(); ++i) {
        /* The public API deliberately exposes lookup rather than table order;
         * walk the known function interval to find every unique entry. */
        (void)i;
    }

    static const u32 functions[] = {
        0x00EBC488u, 0x00EBC51Cu, 0x00EBC664u, 0x00EBC6F8u,
        0x00EBC790u, 0x00EBC824u, 0x00EBC8B8u, 0x00EBCA00u,
        0x00EBCA94u, 0x00EBCB28u, 0x00EBCBBCu, 0x00EBCC50u,
        0x00EBCCE4u, 0x00EBCD78u, 0x00EBCE0Cu, 0x00EBCEA0u,
        0x00EBCF34u, 0x00EBCFC8u, 0x00EBD05Cu, 0x00EBD0F4u,
        0x00EBD188u, 0x00EBD220u, 0x00EBD3F8u, 0x00EBD48Cu,
        0x00EBD5CCu,
    };
    for (u32 i = 0; i < (u32)(sizeof(functions) / sizeof(functions[0])); ++i) {
        const rsx_nr_direct_setter_contract* const c =
            rsx_nr_direct_setter_by_function(functions[i]);
        u32 words[2] = {0, 0};
        CHECK(c != 0);
        CHECK(rsx_nr_direct_setter_packet(functions[i],
                                          0xA5000000u | i, words));
        CHECK(words[0] == ((1u << 18) | c->method));
        CHECK(words[1] == (0xA5000000u | i));
        CHECK(rsx_nr_direct_setter_by_method(c->method) == c);
        for (u32 j = 0; j < i; ++j) {
            const rsx_nr_direct_setter_contract* const prior =
                rsx_nr_direct_setter_by_function(functions[j]);
            CHECK(prior->function_ea != c->function_ea);
            CHECK(prior->method != c->method);
        }
    }

    u32 words[2] = {0xDEADBEEFu, 0xDEADBEEFu};
    CHECK(!rsx_nr_direct_setter_by_function(0x00EBC034u));
    CHECK(!rsx_nr_direct_setter_by_method(0x0050u));
    CHECK(!rsx_nr_direct_setter_packet(0xFFFFFFFFu, 1u, words));
    CHECK(words[0] == 0xDEADBEEFu && words[1] == 0xDEADBEEFu);
    CHECK(!rsx_nr_direct_setter_packet(functions[0], 1u, 0));

    rsx_nr_draw_arrays_contract draw = {0};
    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 5u, 7u, 1u));
    CHECK(draw.primitive == 5u && draw.first == 7u && draw.count == 1u);
    CHECK(draw.batch_count == 1u);
    CHECK(draw.semantic_hash ==
          rsx_nr_draw_hash_batch(rsx_nr_draw_hash_begin(5u, 0), 7u, 1u));

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 6u, 0x1234u, 256u));
    CHECK(draw.batch_count == 1u);
    CHECK(draw.semantic_hash == rsx_nr_draw_hash_batch(
                                    rsx_nr_draw_hash_begin(6u, 0),
                                    0x1234u, 256u));

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 7u, 0xFFFFF0u, 257u));
    CHECK(draw.batch_count == 2u);
    u32 split_hash = rsx_nr_draw_hash_begin(7u, 0);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 0xFFFFF0u, 1u);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 0xFFFFF1u, 256u);
    CHECK(draw.semantic_hash == split_hash);

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 8u, 1000u, 600u));
    CHECK(draw.batch_count == 3u);
    split_hash = rsx_nr_draw_hash_begin(8u, 0);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 1000u, 88u);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 1088u, 256u);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 1344u, 256u);
    CHECK(draw.semantic_hash == split_hash);

    CHECK(!rsx_nr_draw_arrays_contract_init(&draw, 0u, 0u, 1u));
    CHECK(!rsx_nr_draw_arrays_contract_init(&draw, 5u, 0u, 0u));
    CHECK(!rsx_nr_draw_arrays_contract_init(
        &draw, 5u, 0u,
        RSX_NR_DRAW_CONTRACT_MAX_BATCHES * 256u + 1u));
    CHECK(!rsx_nr_draw_arrays_contract_init(0, 5u, 0u, 1u));

    u32 vp_words[RSX_NR_VERTEX_PROGRAM_MAX_WORDS];
    for (u32 i = 0; i < RSX_NR_VERTEX_PROGRAM_MAX_WORDS; ++i)
        vp_words[i] = 0x10203040u ^ (i * 0x01010101u);
    rsx_nr_vertex_program_contract vp = {0};
    CHECK(rsx_nr_vertex_program_contract_init(&vp, 1u, 7u,
                                               0xA5A5u, vp_words));
    CHECK(vp.start_slot == 7u && vp.instruction_count == 1u);
    CHECK(vp.word_count == 4u && vp.attrib_input_mask == 0xA5A5u);
    CHECK(vp.packet_word_count == 12u); /* 3 + (1+4) + 2 + 2 */
    u32 vp_hash = rsx_nr_vertex_program_hash_begin(7u);
    for (u32 i = 0; i < 4u; ++i)
        vp_hash = rsx_nr_vertex_program_hash_word(vp_hash, vp_words[i]);
    CHECK(vp.code_hash == vp_hash);
    vp_hash = rsx_nr_vertex_program_hash_end(vp_hash, 4u, 0xA5A5u);
    CHECK(vp.semantic_hash == vp_hash);

    CHECK(rsx_nr_vertex_program_contract_init(&vp, 8u, 0u,
                                               0xFFFFu, vp_words));
    CHECK(vp.word_count == 32u && vp.packet_word_count == 40u);
    CHECK(rsx_nr_vertex_program_contract_init(&vp, 9u, 4u,
                                               1u, vp_words));
    CHECK(vp.word_count == 36u && vp.packet_word_count == 45u);
    CHECK(rsx_nr_vertex_program_contract_init(
        &vp, RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u, 0u, 1u, vp_words));
    CHECK(vp.word_count == RSX_NR_VERTEX_PROGRAM_MAX_WORDS);
    CHECK(!rsx_nr_vertex_program_contract_init(&vp, 0u, 0u, 0u,
                                                vp_words));
    CHECK(!rsx_nr_vertex_program_contract_init(
        &vp, RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u + 1u, 0u, 0u,
        vp_words));
    CHECK(!rsx_nr_vertex_program_contract_init(&vp, 1u,
        RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u, 0u, vp_words));
    CHECK(!rsx_nr_vertex_program_contract_init(&vp, 2u,
        RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u - 1u, 0u, vp_words));
    CHECK(!rsx_nr_vertex_program_contract_init(0, 1u, 0u, 0u,
                                                vp_words));
    CHECK(!rsx_nr_vertex_program_contract_init(&vp, 1u, 0u, 0u, 0));

    rsx_nr_flip_contract flip = {0};
    CHECK(rsx_nr_flip_contract_init(&flip, 3u, 0, 0u, 0u));
    CHECK(flip.word_count == 4u && flip.flip_word_index == 0u);
    CHECK(flip.words[0] == 0x0004E944u && flip.words[1] == 3u);
    CHECK(flip.words[2] == 0x0004E924u &&
          flip.words[3] == 0x8000010Fu);

    CHECK(rsx_nr_flip_contract_init(&flip, 7u, 1, 0x123u,
                                    0xAABBCCDDu));
    CHECK(flip.word_count == 10u && flip.flip_word_index == 6u);
    CHECK(flip.label_offset == 0x230u &&
          flip.label_value == 0xAABBCCDDu);
    CHECK(flip.words[0] == 0x00040060u &&
          flip.words[1] == 0x66616661u);
    CHECK(flip.words[2] == 0x00040064u && flip.words[3] == 0x230u);
    CHECK(flip.words[4] == 0x00040068u &&
          flip.words[5] == 0xAABBCCDDu);
    CHECK(flip.words[6] == 0x0004E944u && flip.words[7] == 7u);
    CHECK(flip.words[8] == 0x0004E924u &&
          flip.words[9] == 0x8000010Fu);
    CHECK(!rsx_nr_flip_contract_init(&flip, 8u, 0, 0u, 0u));
    CHECK(!rsx_nr_flip_contract_init(0, 0u, 0, 0u, 0u));

    if (failures) {
        fprintf(stderr, "nr producer contract: %d failure(s)\n", failures);
        return 1;
    }
    printf("nr producer contract: 25 setters + draw arrays + vertex program + flip verified\n");
    return 0;
}
