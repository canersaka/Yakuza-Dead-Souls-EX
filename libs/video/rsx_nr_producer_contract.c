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
    out->semantic_hash = hash;
    return 1;
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
