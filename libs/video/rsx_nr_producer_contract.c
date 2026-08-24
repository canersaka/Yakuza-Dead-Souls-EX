#include "rsx_nr_producer_contract.h"

#include <string.h>

/* Audited directly against the lifted wrapper bodies at checkpoint 311410a.
 * Every entry copies gpr4 unchanged into its sole packet argument.  Wrappers
 * that pack multiple arguments, floats, reports, transfers, or synchronization
 * are intentionally absent until they receive their own typed contract. */
static const rsx_nr_direct_setter_contract g_direct_setters[] = {
    {0x00EBC488u, 0x1D8Cu},
    {0x00EBC51Cu, 0x1830u},
    {0x00EBC664u, 0x1834u},
    {0x00EBC6F8u, 0x03B8u},
    {0x00EBC790u, 0x03BCu},
    {0x00EBC824u, 0x1DB4u},
    {0x00EBC8B8u, 0x0378u},
    {0x00EBCA00u, 0x1838u},
    {0x00EBCA94u, 0x147Cu},
    {0x00EBCB28u, 0x1828u},
    {0x00EBCBBCu, 0x182Cu},
    {0x00EBCC50u, 0x032Cu},
    {0x00EBCCE4u, 0x034Cu},
    {0x00EBCD78u, 0x183Cu},
    {0x00EBCE0Cu, 0x0380u},
    {0x00EBCEA0u, 0x0A68u},
    {0x00EBCF34u, 0x1DACu},
    {0x00EBCFC8u, 0x1DB0u},
    {0x00EBD05Cu, 0x1FC0u},
    {0x00EBD0F4u, 0x1FF8u},
    {0x00EBD188u, 0x1FF0u},
    {0x00EBD220u, 0x1FECu},
    {0x00EBD3F8u, 0x17CCu},
    {0x00EBD48Cu, 0x17C8u},
    {0x00EBD5CCu, 0x1804u},
};

u32 rsx_nr_direct_setter_count(void)
{
    return (u32)(sizeof(g_direct_setters) / sizeof(g_direct_setters[0]));
}

const rsx_nr_direct_setter_contract*
rsx_nr_direct_setter_by_function(u32 function_ea)
{
    for (u32 i = 0; i < rsx_nr_direct_setter_count(); ++i) {
        if (g_direct_setters[i].function_ea == function_ea)
            return &g_direct_setters[i];
    }
    return 0;
}

const rsx_nr_direct_setter_contract*
rsx_nr_direct_setter_by_method(u32 method)
{
    for (u32 i = 0; i < rsx_nr_direct_setter_count(); ++i) {
        if (g_direct_setters[i].method == method)
            return &g_direct_setters[i];
    }
    return 0;
}

int rsx_nr_direct_setter_packet(u32 function_ea, u32 value, u32 out[2])
{
    const rsx_nr_direct_setter_contract* const contract =
        rsx_nr_direct_setter_by_function(function_ea);
    if (!contract || !out)
        return 0;
    out[0] = (1u << 18) | contract->method;
    out[1] = value;
    return 1;
}

static u32 draw_hash_word(u32 hash, u32 word)
{
    for (u32 i = 0; i < 4; ++i) {
        hash ^= (word >> (i * 8u)) & 0xFFu;
        hash *= 16777619u;
    }
    return hash;
}

u32 rsx_nr_draw_hash_begin(u32 primitive, u32 indexed)
{
    u32 hash = 2166136261u;
    hash = draw_hash_word(hash, primitive);
    return draw_hash_word(hash, indexed ? 1u : 0u);
}

u32 rsx_nr_draw_hash_batch(u32 hash, u32 first, u32 count)
{
    hash = draw_hash_word(hash, first & 0x00FFFFFFu);
    return draw_hash_word(hash, count);
}

int rsx_nr_draw_arrays_contract_init(rsx_nr_draw_arrays_contract* out,
                                     u32 primitive, u32 first, u32 count)
{
    if (!out || !primitive || !count)
        return 0;
    const u32 full_batches = (count - 1u) >> 8;
    const u32 batches = 1u + full_batches;
    if (batches > RSX_NR_DRAW_CONTRACT_MAX_BATCHES)
        return 0;

    u32 hash = rsx_nr_draw_hash_begin(primitive, 0);
    /* The audited SDK body intentionally emits the remainder first, then
     * full 256-vertex batches. This is observably different from the common
     * full-batches-first normalization when count > 256. */
    const u32 first_count = ((count - 1u) & 0xFFu) + 1u;
    u32 cursor = first;
    hash = rsx_nr_draw_hash_batch(hash, cursor, first_count);
    cursor += first_count;
    for (u32 i = 0; i < full_batches; ++i) {
        hash = rsx_nr_draw_hash_batch(hash, cursor, 256u);
        cursor += 256u;
    }

    out->primitive = primitive;
    out->first = first;
    out->count = count;
    out->batch_count = batches;
    out->packet_word_count = 10u + full_batches +
        (full_batches ? (full_batches + 2046u) / 2047u : 0u);
    out->semantic_hash = hash;
    return 1;
}

u32 rsx_nr_draw_arrays_packet(const rsx_nr_draw_arrays_contract* draw,
                              u32* out, u32 out_capacity)
{
    if (!draw || !out || !draw->primitive || !draw->count ||
        !draw->batch_count ||
        draw->batch_count > RSX_NR_DRAW_CONTRACT_MAX_BATCHES ||
        draw->first > 0x00FFFFFFu ||
        draw->count - 1u > 0x00FFFFFFu - draw->first ||
        draw->packet_word_count > out_capacity)
        return 0;

    u32 at = 0;
    out[at++] = 0x400C1714u;
    out[at++] = 0u;
    out[at++] = 0u;
    out[at++] = 0u;
    out[at++] = 0x00041808u;
    out[at++] = draw->primitive & 0xFFu;

    const u32 full_batches = (draw->count - 1u) >> 8;
    const u32 first_count = ((draw->count - 1u) & 0xFFu) + 1u;
    u32 cursor = draw->first;
    out[at++] = 0x00041814u;
    out[at++] = ((first_count - 1u) << 24) | cursor;
    cursor += first_count;

    u32 remaining = full_batches;
    while (remaining) {
        const u32 group = remaining > 2047u ? 2047u : remaining;
        out[at++] = 0x40000000u | (group << 18) | 0x1814u;
        for (u32 i = 0; i < group; ++i) {
            out[at++] = 0xFF000000u | cursor;
            cursor += 256u;
        }
        remaining -= group;
    }

    out[at++] = 0x00041808u;
    out[at++] = 0u;
    return at == draw->packet_word_count ? at : 0u;
}

u32 rsx_nr_vertex_program_hash_begin(u32 start_slot)
{
    return draw_hash_word(2166136261u, start_slot);
}

u32 rsx_nr_vertex_program_hash_word(u32 hash, u32 word)
{
    return draw_hash_word(hash, word);
}

u32 rsx_nr_vertex_program_hash_end(u32 hash, u32 word_count,
                                   u32 attrib_input_mask)
{
    hash = draw_hash_word(hash, word_count);
    return draw_hash_word(hash, attrib_input_mask);
}

int rsx_nr_vertex_program_contract_init(
    rsx_nr_vertex_program_contract* out, u32 instruction_count,
    u32 start_slot, u32 attrib_input_mask, const u32* words)
{
    if (!out || !words || !instruction_count ||
        instruction_count > RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u ||
        start_slot >= RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u ||
        instruction_count >
            RSX_NR_VERTEX_PROGRAM_MAX_WORDS / 4u - start_slot)
        return 0;

    const u32 word_count = instruction_count * 4u;
    u32 hash = rsx_nr_vertex_program_hash_begin(start_slot);
    for (u32 i = 0; i < word_count; ++i)
        hash = rsx_nr_vertex_program_hash_word(hash, words[i]);
    const u32 code_hash = hash;
    hash = rsx_nr_vertex_program_hash_end(hash, word_count,
                                          attrib_input_mask);

    const u32 full_groups = instruction_count >> 3;
    const u32 remainder_words = (instruction_count & 7u) * 4u;
    const u32 upload_words = full_groups * 33u +
        (remainder_words ? 1u + remainder_words : 0u);

    memset(out, 0, sizeof(*out));
    out->start_slot = start_slot;
    out->instruction_count = instruction_count;
    out->word_count = word_count;
    out->attrib_input_mask = attrib_input_mask;
    out->code_hash = code_hash;
    out->semantic_hash = hash;
    /* Three LOAD+START words, upload packets, ATTRIB_EN, then the always
     * emitted 1EF8 program-limit packet. */
    out->packet_word_count = 3u + upload_words + 2u + 2u;
    return 1;
}

static u64 fragment_hash_bytes(u64 hash, const void* data, u32 size)
{
    const u8* const bytes = (const u8*)data;
    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

u64 rsx_nr_fragment_program_content_hash(const u8* bytes, u32 byte_count)
{
    if (!bytes || !byte_count ||
        byte_count > RSX_NR_FRAGMENT_PROGRAM_MAX_BYTES ||
        (byte_count & 15u) != 0)
        return 0;
    static const u32 tag = 0x31435046u; /* "FPC1" */
    u64 hash = 1469598103934665603ull;
    hash = fragment_hash_bytes(hash, &tag, sizeof(tag));
    return fragment_hash_bytes(hash, bytes, byte_count);
}

u64 rsx_nr_fragment_program_semantic_hash(u64 program_hash,
                                          u32 byte_count, u32 control)
{
    if (!program_hash || !byte_count ||
        byte_count > RSX_NR_FRAGMENT_PROGRAM_MAX_BYTES ||
        (byte_count & 15u) != 0)
        return 0;
    u64 hash = fragment_hash_bytes(program_hash, &byte_count,
                                   sizeof(byte_count));
    return fragment_hash_bytes(hash, &control, sizeof(control));
}

int rsx_nr_fragment_program_contract_init(
    rsx_nr_fragment_program_contract* out, const u8* bytes,
    u32 byte_count, u32 control)
{
    if (!out)
        return 0;
    const u64 content =
        rsx_nr_fragment_program_content_hash(bytes, byte_count);
    const u64 semantic = rsx_nr_fragment_program_semantic_hash(
        content, byte_count, control);
    if (!content || !semantic)
        return 0;
    memset(out, 0, sizeof(*out));
    out->byte_count = byte_count;
    out->control = control;
    out->content_hash = content;
    out->semantic_hash = semantic;
    return 1;
}

void rsx_nr_fragment_binding_init(rsx_nr_fragment_binding_state* state)
{
    if (state)
        memset(state, 0, sizeof(*state));
}

void rsx_nr_fragment_binding_set_program(
    rsx_nr_fragment_binding_state* state, u32 program_word)
{
    if (!state)
        return;
    state->program_word = program_word;
    state->valid_mask |= 1u;
}

void rsx_nr_fragment_binding_set_control(
    rsx_nr_fragment_binding_state* state, u32 control)
{
    if (!state)
        return;
    state->control = control;
    state->valid_mask |= 2u;
}

int rsx_nr_fragment_binding_snapshot(
    const rsx_nr_fragment_binding_state* state,
    u32* program_word, u32* control)
{
    if (!state || !program_word || !control ||
        (state->valid_mask & 3u) != 3u)
        return 0;
    *program_word = state->program_word;
    *control = state->control;
    return 1;
}

int rsx_nr_flip_contract_init(rsx_nr_flip_contract* out, u32 buffer_id,
                              int wait_for_label, u32 label_index,
                              u32 label_value)
{
    if (!out || buffer_id >= 8u)
        return 0;
    memset(out, 0, sizeof(*out));
    out->buffer_id = buffer_id;
    out->wait_for_label = wait_for_label ? 1u : 0u;
    out->label_offset = (label_index & 0xFFu) * 0x10u;
    out->label_value = label_value;

    u32 at = 0;
    if (wait_for_label) {
        out->words[at++] = 0x00040060u;
        out->words[at++] = 0x66616661u;
        out->words[at++] = 0x00040064u;
        out->words[at++] = out->label_offset;
        out->words[at++] = 0x00040068u;
        out->words[at++] = label_value;
    }
    out->flip_word_index = at;
    out->words[at++] = 0x0004E944u;
    out->words[at++] = buffer_id;
    out->words[at++] = 0x0004E924u;
    out->words[at++] = 0x8000010Fu;
    out->word_count = at;
    return 1;
}

int rsx_nr_main_report_io_range(u32 offset, u32 size, u32* io_offset)
{
    const u32 aperture_size = 0x01000000u;
    if (!io_offset || !size || offset >= aperture_size ||
        size > aperture_size - offset)
        return 0;
    *io_offset = RSX_NR_MAIN_REPORT_IO_BASE + offset;
    return 1;
}

rsx_nr_fifo_range_status rsx_nr_fifo_section_range_status(
    u32 pc, u32 size, u32 put, u32 call_return, u32 ring_size)
{
    if (!size || (pc & 3u) || (size & 3u) ||
        !ring_size || (ring_size & (ring_size - 1u)) ||
        pc > 0xFFFFFFFFu - size)
        return RSX_NR_FIFO_RANGE_WINDOW;

    /* CALL publication is the visibility boundary for its target list.  PUT
     * continues to describe only the caller's primary ring and can be either
     * numerically before or after a low-address called segment. */
    if (call_return != 0xFFFFFFFFu)
        return RSX_NR_FIFO_RANGE_READY;

    if (pc >= ring_size || put >= ring_size || size > ring_size - pc)
        return RSX_NR_FIFO_RANGE_WINDOW;
    const u32 available = (put - pc + ring_size) & (ring_size - 1u);
    if (available < size)
        return RSX_NR_FIFO_RANGE_NOT_READY;
    return RSX_NR_FIFO_RANGE_READY;
}

void rsx_nr_fifo_visit_reset(rsx_nr_fifo_visit_set* set)
{
    if (!set)
        return;
    set->generation++;
    if (!set->generation) {
        memset(set->stamp, 0, sizeof(set->stamp));
        set->generation = 1u;
    }
}

int rsx_nr_fifo_visit_note(rsx_nr_fifo_visit_set* set, u32 pc, u32 ret)
{
    if (!set || !set->generation)
        return -1;
    const u32 mask = RSX_NR_FIFO_VISIT_CAPACITY - 1u;
    u32 index = ((pc >> 2) ^ (ret * 0x9E3779B9u)) & mask;
    for (u32 probe = 0; probe < RSX_NR_FIFO_VISIT_CAPACITY; ++probe) {
        if (set->stamp[index] != set->generation) {
            set->pc[index] = pc;
            set->ret[index] = ret;
            set->stamp[index] = set->generation;
            return 1;
        }
        if (set->pc[index] == pc && set->ret[index] == ret)
            return 0;
        index = (index + 1u) & mask;
    }
    return -1;
}

int rsx_nr_fifo_visit_contains(
    const rsx_nr_fifo_visit_set* set, u32 pc, u32 ret)
{
    if (!set || !set->generation)
        return 0;
    const u32 mask = RSX_NR_FIFO_VISIT_CAPACITY - 1u;
    u32 index = ((pc >> 2) ^ (ret * 0x9E3779B9u)) & mask;
    for (u32 probe = 0; probe < RSX_NR_FIFO_VISIT_CAPACITY; ++probe) {
        if (set->stamp[index] != set->generation)
            return 0;
        if (set->pc[index] == pc && set->ret[index] == ret)
            return 1;
        index = (index + 1u) & mask;
    }
    return 0;
}

int rsx_nr_complete_section_family_allowed(
    u32 enabled_families, u32 action_family, u32 section_draw_count)
{
    if (enabled_families & action_family)
        return 1;
    return action_family == RSX_NR_GRAPHICS_FAMILY_CLEAR &&
        section_draw_count != 0u &&
        (enabled_families & RSX_NR_GRAPHICS_FAMILY_DRAW) != 0u;
}

int rsx_nr_yz_unproven_shadow_depth_producer(
    u32 zeta_location, u32 zeta_offset, u32 color_mask,
    u32 depth_write_enable)
{
    return zeta_location == 0u && color_mask == 0u &&
        depth_write_enable != 0u &&
        (zeta_offset == RSX_NR_YZ_SHADOW_ZETA0 ||
         zeta_offset == RSX_NR_YZ_SHADOW_ZETA1);
}

int rsx_nr_yz_unproven_shadow_depth_consumer(
    u32 enabled, u32 texture_location, u32 texture_offset,
    u32 texture_format)
{
    const u32 base_format = (texture_format & 0x9Fu) & ~0x40u;
    return enabled != 0u && texture_location == 0u &&
        base_format == 0x90u &&
        (texture_offset == RSX_NR_YZ_SHADOW_ZETA0 ||
         texture_offset == RSX_NR_YZ_SHADOW_ZETA1);
}

int rsx_nr_fifo_frame_boundary(u32 method, u32 arg)
{
    return method == 0xE924u && arg == 0x8000010Fu;
}

int rsx_nr_legacy_gpu_action(u32 method, u32 arg)
{
    return (method == 0x1808u && arg == 0u) || method == 0x1D94u ||
        method == 0x2328u || method == 0xC40Cu ||
        method == 0x0050u || method == 0x006Cu || method == 0x0110u ||
        method == 0x17C8u || method == 0x1800u ||
        method == 0x1D70u || method == 0x1D74u ||
        (method >= 0xA400u && method <= 0xAAFCu) ||
        (method >= 0xE920u && method <= 0xE95Cu);
}
