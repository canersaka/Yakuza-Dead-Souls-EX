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
    CHECK(draw.packet_word_count == 10u);
    CHECK(draw.semantic_hash ==
          rsx_nr_draw_hash_batch(rsx_nr_draw_hash_begin(5u, 0), 7u, 1u));

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 6u, 0x1234u, 256u));
    CHECK(draw.batch_count == 1u);
    CHECK(draw.packet_word_count == 10u);
    CHECK(draw.semantic_hash == rsx_nr_draw_hash_batch(
                                    rsx_nr_draw_hash_begin(6u, 0),
                                    0x1234u, 256u));

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 7u, 0xFFFFF0u, 257u));
    CHECK(draw.batch_count == 2u);
    CHECK(draw.packet_word_count == 12u);
    u32 split_hash = rsx_nr_draw_hash_begin(7u, 0);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 0xFFFFF0u, 1u);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 0xFFFFF1u, 256u);
    CHECK(draw.semantic_hash == split_hash);

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 8u, 1000u, 600u));
    CHECK(draw.batch_count == 3u);
    CHECK(draw.packet_word_count == 13u);
    split_hash = rsx_nr_draw_hash_begin(8u, 0);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 1000u, 88u);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 1088u, 256u);
    split_hash = rsx_nr_draw_hash_batch(split_hash, 1344u, 256u);
    CHECK(draw.semantic_hash == split_hash);

    u32 draw_packet[32] = {0};
    CHECK(rsx_nr_draw_arrays_packet(&draw, draw_packet, 32u) == 13u);
    CHECK(draw_packet[0] == 0x400C1714u &&
          draw_packet[1] == 0u && draw_packet[2] == 0u &&
          draw_packet[3] == 0u);
    CHECK(draw_packet[4] == 0x00041808u && draw_packet[5] == 8u);
    CHECK(draw_packet[6] == 0x00041814u &&
          draw_packet[7] == (0x57000000u | 1000u));
    CHECK(draw_packet[8] == 0x40081814u &&
          draw_packet[9] == (0xFF000000u | 1088u) &&
          draw_packet[10] == (0xFF000000u | 1344u));
    CHECK(draw_packet[11] == 0x00041808u && draw_packet[12] == 0u);
    CHECK(!rsx_nr_draw_arrays_packet(&draw, draw_packet, 12u));

    /* A 2048-full-batch boundary starts a second legal non-increment packet
     * instead of overflowing the 11-bit method count. */
    CHECK(rsx_nr_draw_arrays_contract_init(
        &draw, 5u, 0u, 2048u * 256u + 1u));
    CHECK(draw.batch_count == 2049u && draw.packet_word_count == 2060u);
    u32 large_packet[2060];
    CHECK(rsx_nr_draw_arrays_packet(&draw, large_packet, 2060u) == 2060u);
    CHECK(large_packet[8] == 0x5FFC1814u);
    CHECK(large_packet[2056] == 0x40041814u);
    CHECK(large_packet[2058] == 0x00041808u &&
          large_packet[2059] == 0u);

    CHECK(rsx_nr_draw_arrays_contract_init(&draw, 5u, 0xFFFFF0u, 257u));
    CHECK(!rsx_nr_draw_arrays_packet(&draw, draw_packet, 32u));

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

    u8 fp_bytes[64];
    for (u32 i = 0; i < (u32)sizeof(fp_bytes); ++i)
        fp_bytes[i] = (u8)(i * 37u + 11u);
    rsx_nr_fragment_program_contract fp = {0};
    CHECK(rsx_nr_fragment_program_contract_init(
        &fp, fp_bytes, sizeof(fp_bytes), 0x00000440u));
    CHECK(fp.byte_count == sizeof(fp_bytes));
    CHECK(fp.control == 0x00000440u);
    CHECK(fp.content_hash == rsx_nr_fragment_program_content_hash(
                                  fp_bytes, sizeof(fp_bytes)));
    CHECK(fp.semantic_hash == rsx_nr_fragment_program_semantic_hash(
                                   fp.content_hash, sizeof(fp_bytes),
                                   0x00000440u));
    const u64 original_content = fp.content_hash;
    const u64 original_semantic = fp.semantic_hash;
    fp_bytes[31] ^= 0x80u;
    CHECK(rsx_nr_fragment_program_contract_init(
        &fp, fp_bytes, sizeof(fp_bytes), 0x00000440u));
    CHECK(fp.content_hash != original_content);
    CHECK(fp.semantic_hash != original_semantic);
    const u64 patched_content = fp.content_hash;
    const u64 patched_semantic = fp.semantic_hash;
    CHECK(rsx_nr_fragment_program_contract_init(
        &fp, fp_bytes, sizeof(fp_bytes), 0x00000400u));
    CHECK(fp.content_hash == patched_content);
    CHECK(fp.semantic_hash != patched_semantic);
    CHECK(!rsx_nr_fragment_program_contract_init(&fp, fp_bytes, 15u, 0u));
    CHECK(!rsx_nr_fragment_program_contract_init(
        &fp, fp_bytes, RSX_NR_FRAGMENT_PROGRAM_MAX_BYTES + 16u, 0u));
    CHECK(!rsx_nr_fragment_program_contract_init(
        &fp, 0, sizeof(fp_bytes), 0u));
    CHECK(!rsx_nr_fragment_program_contract_init(
        0, fp_bytes, sizeof(fp_bytes), 0u));

    rsx_nr_fragment_binding_state fp_binding;
    u32 bound_program = 0xFFFFFFFFu;
    u32 bound_control = 0xFFFFFFFFu;
    rsx_nr_fragment_binding_init(&fp_binding);
    CHECK(!rsx_nr_fragment_binding_snapshot(
        &fp_binding, &bound_program, &bound_control));
    rsx_nr_fragment_binding_set_program(&fp_binding, 0x00123402u);
    CHECK(!rsx_nr_fragment_binding_snapshot(
        &fp_binding, &bound_program, &bound_control));
    rsx_nr_fragment_binding_set_program(&fp_binding, 0x00567801u);
    rsx_nr_fragment_binding_set_control(&fp_binding, 0x02008400u);
    CHECK(rsx_nr_fragment_binding_snapshot(
        &fp_binding, &bound_program, &bound_control));
    CHECK(bound_program == 0x00567801u &&
          bound_control == 0x02008400u);
    /* Neither setter consumes the other persistent state. */
    rsx_nr_fragment_binding_set_program(&fp_binding, 0x00ABC002u);
    CHECK(rsx_nr_fragment_binding_snapshot(
        &fp_binding, &bound_program, &bound_control));
    CHECK(bound_program == 0x00ABC002u &&
          bound_control == 0x02008400u);
    rsx_nr_fragment_binding_set_control(&fp_binding, 0x03008400u);
    CHECK(rsx_nr_fragment_binding_snapshot(
        &fp_binding, &bound_program, &bound_control));
    CHECK(bound_program == 0x00ABC002u &&
          bound_control == 0x03008400u);
    CHECK(!rsx_nr_fragment_binding_snapshot(
        0, &bound_program, &bound_control));
    CHECK(!rsx_nr_fragment_binding_snapshot(
        &fp_binding, 0, &bound_control));

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

    u32 report_io = 0;
    CHECK(rsx_nr_main_report_io_range(0x45C0u, 16u, &report_io));
    CHECK(report_io == 0x0E0045C0u);
    CHECK(report_io != 0x000045C0u); /* never alias the FIFO low offset */
    CHECK(rsx_nr_main_report_io_range(0x00FFFFF0u, 16u, &report_io));
    CHECK(report_io == 0x0EFFFFF0u);
    CHECK(!rsx_nr_main_report_io_range(0x00FFFFF1u, 16u, &report_io));
    CHECK(!rsx_nr_main_report_io_range(0u, 0u, &report_io));
    CHECK(!rsx_nr_main_report_io_range(0u, 16u, 0));

    /* Primary packets are bounded by the exact modulo GET-to-PUT span,
     * including partial publication and packet crossing. A backlog larger
     * than half the ring remains valid; the half-ring heuristic belongs to
     * recovery proofs, not normal consumption. A called display list is
     * already published by its CALL and must not be compared to caller PUT. */
    const u32 fifo_ring = 0x800000u;
    CHECK(rsx_nr_fifo_section_range_status(
              0x1000u, 8u, 0x1008u, 0xFFFFFFFFu, fifo_ring) ==
          RSX_NR_FIFO_RANGE_READY);
    CHECK(rsx_nr_fifo_section_range_status(
              0x1000u, 12u, 0x1008u, 0xFFFFFFFFu, fifo_ring) ==
          RSX_NR_FIFO_RANGE_NOT_READY);
    CHECK(rsx_nr_fifo_section_range_status(
              0x1000u, 4u, 0x700000u, 0xFFFFFFFFu, fifo_ring) ==
          RSX_NR_FIFO_RANGE_READY);
    CHECK(rsx_nr_fifo_section_range_status(
              0x7FFFFCu, 8u, 0x1000u, 0xFFFFFFFFu, fifo_ring) ==
          RSX_NR_FIFO_RANGE_WINDOW);
    CHECK(rsx_nr_fifo_section_range_status(
              0x2000u, 16u, 0x1000u, 0x700100u, fifo_ring) ==
          RSX_NR_FIFO_RANGE_READY);
    CHECK(rsx_nr_fifo_section_range_status(
              0x1104D00u, 16u, 0x1000u, 0x700100u, fifo_ring) ==
          RSX_NR_FIFO_RANGE_READY);
    CHECK(rsx_nr_fifo_section_range_status(
              0xFFFFFFFCu, 8u, 0x1000u, 0x700100u, fifo_ring) ==
          RSX_NR_FIFO_RANGE_WINDOW);
    CHECK(rsx_nr_fifo_section_range_status(
              0x1000u, 0u, 0x1008u, 0xFFFFFFFFu, fifo_ring) ==
          RSX_NR_FIFO_RANGE_WINDOW);

    /* Multi-packet cycles are detected by exact PC+return identity in O(1)
     * generations. A million retries cannot burn the scanner step budget. */
    {
        static rsx_nr_fifo_visit_set visits;
        rsx_nr_fifo_visit_reset(&visits);
        CHECK(rsx_nr_fifo_visit_note(&visits, 0x1000u, 0xFFFFFFFFu) == 1);
        CHECK(rsx_nr_fifo_visit_note(&visits, 0x1004u, 0xFFFFFFFFu) == 1);
        CHECK(rsx_nr_fifo_visit_note(&visits, 0x1000u, 0x2000u) == 1);
        for (u32 i = 0; i < 16384u; ++i)
            CHECK(rsx_nr_fifo_visit_note(
                      &visits, 0x10000u + i * 4u,
                      0x80000000u + i * 4u) == 1);
        for (u32 i = 0; i < 1000000u; ++i)
            CHECK(rsx_nr_fifo_visit_note(
                      &visits, 0x1000u, 0xFFFFFFFFu) == 0);
        rsx_nr_fifo_visit_reset(&visits);
        CHECK(rsx_nr_fifo_visit_note(&visits, 0x1000u, 0xFFFFFFFFu) == 1);
        visits.generation = 0xFFFFFFFFu;
        rsx_nr_fifo_visit_reset(&visits);
        CHECK(visits.generation == 1u &&
              rsx_nr_fifo_visit_note(
                  &visits, 0x1000u, 0xFFFFFFFFu) == 1);
    }

    /* A rejected flow/capacity path is a conservative legacy-only cache.
     * A million retries through its exact nodes require no reinsertion or
     * scanner reset; leaving the path is detected exactly. */
    {
        static rsx_nr_fifo_visit_set rejected;
        const u32 pc[4] = {0x2000u, 0x2100u, 0x2200u, 0x2300u};
        const u32 ret[4] = {
            0xFFFFFFFFu, 0x3000u, 0x3000u, 0xFFFFFFFFu
        };
        rsx_nr_fifo_visit_reset(&rejected);
        for (u32 i = 0; i < 4u; ++i)
            CHECK(rsx_nr_fifo_visit_note(&rejected, pc[i], ret[i]) == 1);
        for (u32 i = 0; i < 1000000u; ++i)
            CHECK(rsx_nr_fifo_visit_contains(
                      &rejected, pc[i & 3u], ret[i & 3u]));
        CHECK(!rsx_nr_fifo_visit_contains(
                  &rejected, 0x2400u, 0xFFFFFFFFu));
        CHECK(!rsx_nr_fifo_visit_contains(
                  &rejected, 0x2000u, 0x3000u));
        rsx_nr_fifo_visit_reset(&rejected);
        CHECK(!rsx_nr_fifo_visit_contains(
                  &rejected, 0x2000u, 0xFFFFFFFFu));
    }

    /* Draw-only mode can own a complete clear+draw render pass, but never a
     * standalone clear. Explicit clear-only and ordinary draw admission keep
     * their direct family behavior. */
    CHECK(rsx_nr_complete_section_family_allowed(
              RSX_NR_GRAPHICS_FAMILY_DRAW,
              RSX_NR_GRAPHICS_FAMILY_CLEAR, 1u));
    CHECK(!rsx_nr_complete_section_family_allowed(
              RSX_NR_GRAPHICS_FAMILY_DRAW,
              RSX_NR_GRAPHICS_FAMILY_CLEAR, 0u));
    CHECK(rsx_nr_complete_section_family_allowed(
              RSX_NR_GRAPHICS_FAMILY_CLEAR,
              RSX_NR_GRAPHICS_FAMILY_CLEAR, 0u));
    CHECK(rsx_nr_complete_section_family_allowed(
              RSX_NR_GRAPHICS_FAMILY_DRAW,
              RSX_NR_GRAPHICS_FAMILY_DRAW, 0u));
    CHECK(!rsx_nr_complete_section_family_allowed(
              0u, RSX_NR_GRAPHICS_FAMILY_DRAW, 1u));

    /* Per-action ownership is forbidden: only the complete queue/head flip
     * pair closes a transactional native frame. */
    CHECK(!rsx_nr_fifo_frame_boundary(0x1808u, 0u));
    CHECK(!rsx_nr_fifo_frame_boundary(0x1D94u, 0xF3u));
    CHECK(!rsx_nr_fifo_frame_boundary(0x2328u, 0u));
    CHECK(!rsx_nr_fifo_frame_boundary(0x0110u, 0u));
    CHECK(!rsx_nr_fifo_frame_boundary(0x17C8u, 0u));
    CHECK(!rsx_nr_fifo_frame_boundary(0xEB00u, 1u));
    CHECK(!rsx_nr_fifo_frame_boundary(0xE944u, 3u));
    CHECK(!rsx_nr_fifo_frame_boundary(0xE924u, 0u));
    CHECK(rsx_nr_fifo_frame_boundary(0xE924u, 0x8000010Fu));

    CHECK(rsx_nr_legacy_gpu_action(0x1808u, 0u));
    CHECK(!rsx_nr_legacy_gpu_action(0x1808u, 1u));
    CHECK(rsx_nr_legacy_gpu_action(0x1D94u, 0xF3u));
    CHECK(rsx_nr_legacy_gpu_action(0xA400u, 0u));
    CHECK(rsx_nr_legacy_gpu_action(0xAAFCu, 0u));
    CHECK(!rsx_nr_legacy_gpu_action(0xA3FCu, 0u));
    CHECK(!rsx_nr_legacy_gpu_action(0xAB00u, 0u));
    CHECK(!rsx_nr_legacy_gpu_action(0x1D90u, 0u));

    if (failures) {
        fprintf(stderr, "nr producer contract: %d failure(s)\n", failures);
        return 1;
    }
    printf("nr producer contract: 25 setters + draw arrays + vertex/fragment programs + flip verified\n");
    return 0;
}
