/*
 * ps3recomp - NIR emitter implementation. See rsx_nir_emitter.h.
 */

#include "rsx_nir_emitter.h"

#include <string.h>

void rsx_nir_emitter_init(rsx_nir_emitter* em, const rsx_nir_sink* out)
{
    memset(em, 0, sizeof(*em));
    em->out = *out;
    rsx_nir_pipeline_init(&em->shadow);
    rsx_nir_pipeline_init(&em->pending);
}

void rsx_nir_emitter_init_stream(rsx_nir_emitter* em, rsx_nir_stream* out)
{
    rsx_nir_sink k = rsx_nir_stream_sink(out);
    rsx_nir_emitter_init(em, &k);
}

/* ---- staging setters --------------------------------------------------- */

void rsx_nir_em_surface(rsx_nir_emitter* em, const rsx_nir_surface* s)
{
    em->pending.surface = *s;
}

void rsx_nir_em_viewport(rsx_nir_emitter* em, const rsx_nir_viewport* v)
{
    em->pending.viewport = *v;
}

void rsx_nir_em_scissor(rsx_nir_emitter* em, const rsx_nir_scissor* s)
{
    em->pending.scissor = *s;
}

void rsx_nir_em_raster(rsx_nir_emitter* em, const rsx_nir_raster* r)
{
    em->pending.raster = *r;
}

void rsx_nir_em_depth_stencil(rsx_nir_emitter* em, const rsx_nir_depth_stencil* d)
{
    em->pending.depth_stencil = *d;
}

void rsx_nir_em_blend(rsx_nir_emitter* em, const rsx_nir_blend* b)
{
    em->pending.blend = *b;
}

void rsx_nir_em_render_condition(rsx_nir_emitter* em,
                                 const rsx_nir_render_condition* c)
{
    em->pending.render_condition = *c;
}

void rsx_nir_em_fragment_program(rsx_nir_emitter* em, const rsx_nir_fragment_program* f)
{
    em->pending.fragment_program = *f;
}

void rsx_nir_em_vertex_bindings(rsx_nir_emitter* em, const rsx_nir_vertex_bindings* v)
{
    em->pending.vertex_bindings = *v;
}

void rsx_nir_em_index_binding(rsx_nir_emitter* em, const rsx_nir_index_binding* i)
{
    em->pending.index_binding = *i;
}

void rsx_nir_em_texture(rsx_nir_emitter* em, u32 unit, const rsx_nir_texture* t)
{
    if (unit < RSX_NIR_NUM_TEXTURES)
        em->pending.textures[unit] = *t;
}

void rsx_nir_em_vertex_texture(rsx_nir_emitter* em, u32 unit, const rsx_nir_texture* t)
{
    if (unit < RSX_NIR_NUM_VERTEX_TEXTURES)
        em->pending.vertex_textures[unit] = *t;
}

void rsx_nir_em_constants(rsx_nir_emitter* em, u32 first_slot, u32 slot_count,
                          const u32* words)
{
    for (u32 i = 0; i < slot_count; i++) {
        u32 slot = first_slot + i;
        if (slot >= RSX_NIR_NUM_CONSTANTS)
            break;
        memcpy(em->pending.constants[slot], words + i * 4, 16);
        em->pending.constants_written[slot] = 1;
        em->const_dirty[slot] = 1;
    }
}

void rsx_nir_em_vertex_program(rsx_nir_emitter* em, u32 start_slot,
                               const u32* words, u32 word_count,
                               u32 attrib_input_mask, u32 attrib_output_mask,
                               u32 branch_bits)
{
    if (word_count > RSX_NIR_VP_MAX_WORDS)
        word_count = RSX_NIR_VP_MAX_WORDS;
    rsx_nir_vertex_program* vp = &em->pending.vertex_program;
    vp->start_slot = start_slot;
    vp->word_count = word_count;
    vp->hash = rsx_nir_hash_words(words, word_count);
    vp->attrib_input_mask = attrib_input_mask;
    vp->attrib_output_mask = attrib_output_mask;
    vp->branch_bits = branch_bits;
    vp->words_ofs = 0;   /* assigned at flush when the op is emitted */
    if (word_count)
        memcpy(em->vp_words, words, (size_t)word_count * 4);
}

/* ---- flush ------------------------------------------------------------- */

static void push_group(rsx_nir_emitter* em, u32 kind, u32 unit,
                       const void* payload, u32 payload_size)
{
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = kind;
    op.unit = unit;
    memcpy(&op.u, payload, payload_size);
    em->out.push(em->out.user, &op);
}

#define FLUSH_GROUP(field, KIND)                                             \
    do {                                                                     \
        if (force || memcmp(&em->pending.field, &em->shadow.field,           \
                            sizeof(em->pending.field))) {                    \
            push_group(em, KIND, 0, &em->pending.field,                      \
                       sizeof(em->pending.field));                           \
            em->shadow.field = em->pending.field;                            \
        }                                                                    \
    } while (0)

static void flush_state(rsx_nir_emitter* em)
{
    const int force = !em->primed;
    em->primed = 1;

    FLUSH_GROUP(surface, RSX_NIR_OP_SET_SURFACE);
    FLUSH_GROUP(viewport, RSX_NIR_OP_SET_VIEWPORT);
    FLUSH_GROUP(scissor, RSX_NIR_OP_SET_SCISSOR);
    FLUSH_GROUP(raster, RSX_NIR_OP_SET_RASTER);
    FLUSH_GROUP(depth_stencil, RSX_NIR_OP_SET_DEPTH_STENCIL);
    FLUSH_GROUP(blend, RSX_NIR_OP_SET_BLEND);
    /* Disabled is the architectural reset default, so an all-default first
     * action need not spend an op on it. Any enabled condition (including one
     * staged before the first action) still emits before that action. */
    if (memcmp(&em->pending.render_condition,
               &em->shadow.render_condition,
               sizeof(em->pending.render_condition))) {
        push_group(em, RSX_NIR_OP_SET_RENDER_CONDITION, 0,
                   &em->pending.render_condition,
                   sizeof(em->pending.render_condition));
        em->shadow.render_condition = em->pending.render_condition;
    }
    FLUSH_GROUP(fragment_program, RSX_NIR_OP_SET_FRAGMENT_PROGRAM);
    FLUSH_GROUP(vertex_bindings, RSX_NIR_OP_SET_VERTEX_BINDINGS);
    FLUSH_GROUP(index_binding, RSX_NIR_OP_SET_INDEX_BINDING);

    for (u32 t = 0; t < RSX_NIR_NUM_TEXTURES; t++) {
        if (force || memcmp(&em->pending.textures[t], &em->shadow.textures[t],
                            sizeof(rsx_nir_texture))) {
            push_group(em, RSX_NIR_OP_SET_TEXTURE, t,
                       &em->pending.textures[t], sizeof(rsx_nir_texture));
            em->shadow.textures[t] = em->pending.textures[t];
        }
    }
    for (u32 t = 0; t < RSX_NIR_NUM_VERTEX_TEXTURES; t++) {
        if (force || memcmp(&em->pending.vertex_textures[t],
                            &em->shadow.vertex_textures[t],
                            sizeof(rsx_nir_texture))) {
            push_group(em, RSX_NIR_OP_SET_VERTEX_TEXTURE, t,
                       &em->pending.vertex_textures[t], sizeof(rsx_nir_texture));
            em->shadow.vertex_textures[t] = em->pending.vertex_textures[t];
        }
    }

    /* vertex program: emit when identity changed (or first action) */
    if (force ||
        em->pending.vertex_program.hash != em->shadow.vertex_program.hash ||
        em->pending.vertex_program.word_count != em->shadow.vertex_program.word_count ||
        em->pending.vertex_program.start_slot != em->shadow.vertex_program.start_slot ||
        em->pending.vertex_program.attrib_input_mask != em->shadow.vertex_program.attrib_input_mask ||
        em->pending.vertex_program.attrib_output_mask != em->shadow.vertex_program.attrib_output_mask ||
        em->pending.vertex_program.branch_bits != em->shadow.vertex_program.branch_bits) {
        rsx_nir_vertex_program vp = em->pending.vertex_program;
        vp.words_ofs = em->out.side_push(em->out.user, em->vp_words,
                                         vp.word_count);
        push_group(em, RSX_NIR_OP_SET_VERTEX_PROGRAM, 0, &vp, sizeof(vp));
        em->pending.vertex_program.words_ofs = vp.words_ofs;
        em->shadow.vertex_program = vp;
    }

    /* constants: contiguous dirty runs. A slot counts as dirty when marked
     * AND its folded value or written-flag differs from the shadow (so
     * rewriting an identical value emits nothing). */
    u32 run_start = 0;
    int in_run = 0;
    for (u32 slot = 0; slot <= RSX_NIR_NUM_CONSTANTS; slot++) {
        int dirty = 0;
        if (slot < RSX_NIR_NUM_CONSTANTS) {
            if (force)
                dirty = em->pending.constants_written[slot] != 0;
            else if (em->const_dirty[slot])
                dirty = em->pending.constants_written[slot] !=
                            em->shadow.constants_written[slot] ||
                        memcmp(em->pending.constants[slot],
                               em->shadow.constants[slot], 16) != 0;
        }
        if (dirty && !in_run) {
            run_start = slot;
            in_run = 1;
        } else if (!dirty && in_run) {
            rsx_nir_constants c;
            memset(&c, 0, sizeof(c));
            c.first_slot = run_start;
            c.slot_count = slot - run_start;
            c.words_ofs = em->out.side_push(em->out.user,
                                            &em->pending.constants[run_start][0],
                                            c.slot_count * 4);
            push_group(em, RSX_NIR_OP_SET_CONSTANTS, 0, &c, sizeof(c));
            for (u32 j = run_start; j < slot; j++) {
                memcpy(em->shadow.constants[j], em->pending.constants[j], 16);
                em->shadow.constants_written[j] =
                    em->pending.constants_written[j];
            }
            in_run = 0;
        }
    }
    memset(em->const_dirty, 0, sizeof(em->const_dirty));
}

/* ---- actions ----------------------------------------------------------- */

static void push_action(rsx_nir_emitter* em, u32 kind,
                        const void* payload, u32 payload_size)
{
    flush_state(em);
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = kind;
    memcpy(&op.u, payload, payload_size);
    em->out.push(em->out.user, &op);
}

void rsx_nir_em_clear(rsx_nir_emitter* em, u32 mask, u32 color_value,
                      u32 depth_value, u32 stencil_value)
{
    rsx_nir_clear c;
    memset(&c, 0, sizeof(c));
    c.mask = mask;
    c.color_value = color_value;
    c.depth_value = depth_value;
    c.stencil_value = stencil_value;
    push_action(em, RSX_NIR_OP_CLEAR, &c, sizeof(c));
}

void rsx_nir_em_draw(rsx_nir_emitter* em, u32 primitive, u32 indexed,
                     const u32* batches, u32 batch_count)
{
    rsx_nir_draw d;
    memset(&d, 0, sizeof(d));
    d.primitive = primitive;
    d.indexed = indexed;
    d.batch_count = batch_count;
    flush_state(em);
    d.batches_ofs = em->out.side_push(em->out.user, batches, batch_count * 2);
    for (u32 i = 0; i < batch_count; i++)
        d.total_count += batches[i * 2 + 1];
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = RSX_NIR_OP_DRAW;
    op.u.draw = d;
    em->out.push(em->out.user, &op);
}

void rsx_nir_em_transfer(rsx_nir_emitter* em, const rsx_nir_transfer* t,
                         const u32* words)
{
    rsx_nir_transfer x = *t;
    flush_state(em);
    x.words_ofs = x.word_count
        ? em->out.side_push(em->out.user, words, x.word_count)
        : 0;
    rsx_nir_op op;
    memset(&op, 0, sizeof(op));
    op.kind = RSX_NIR_OP_TRANSFER;
    op.u.transfer = x;
    em->out.push(em->out.user, &op);
}

void rsx_nir_em_semaphore_release(rsx_nir_emitter* em, u32 dma_context,
                                  u32 offset, u32 value, u32 texture_read)
{
    rsx_nir_semaphore s;
    memset(&s, 0, sizeof(s));
    s.dma_context = dma_context;
    s.offset = offset;
    s.value = value;
    s.texture_read = texture_read;
    push_action(em, RSX_NIR_OP_SEMAPHORE_RELEASE, &s, sizeof(s));
}

void rsx_nir_em_semaphore_acquire(rsx_nir_emitter* em, u32 dma_context,
                                  u32 offset, u32 value)
{
    rsx_nir_semaphore s;
    memset(&s, 0, sizeof(s));
    s.dma_context = dma_context;
    s.offset = offset;
    s.value = value;
    push_action(em, RSX_NIR_OP_SEMAPHORE_ACQUIRE, &s, sizeof(s));
}

void rsx_nir_em_report(rsx_nir_emitter* em, u32 kind, u32 arg, u32 dma_report)
{
    rsx_nir_report r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.arg = arg;
    r.dma_report = dma_report;
    push_action(em, RSX_NIR_OP_REPORT, &r, sizeof(r));
}

void rsx_nir_em_barrier(rsx_nir_emitter* em, u32 kind)
{
    rsx_nir_barrier b;
    memset(&b, 0, sizeof(b));
    b.kind = kind;
    push_action(em, RSX_NIR_OP_BARRIER, &b, sizeof(b));
}

void rsx_nir_em_set_reference(rsx_nir_emitter* em, u32 value)
{
    rsx_nir_reference r;
    memset(&r, 0, sizeof(r));
    r.value = value;
    push_action(em, RSX_NIR_OP_SET_REFERENCE, &r, sizeof(r));
}

void rsx_nir_em_user_command(rsx_nir_emitter* em, u32 cause)
{
    rsx_nir_user_command u;
    memset(&u, 0, sizeof(u));
    u.cause = cause;
    push_action(em, RSX_NIR_OP_USER_COMMAND, &u, sizeof(u));
}

void rsx_nir_em_token_signal(rsx_nir_emitter* em, u32 token, u32 value)
{
    rsx_nir_token t;
    memset(&t, 0, sizeof(t));
    t.token = token;
    t.value = value;
    push_action(em, RSX_NIR_OP_TOKEN_SIGNAL, &t, sizeof(t));
}

void rsx_nir_em_token_wait(rsx_nir_emitter* em, u32 token, u32 value)
{
    rsx_nir_token t;
    memset(&t, 0, sizeof(t));
    t.token = token;
    t.value = value;
    push_action(em, RSX_NIR_OP_TOKEN_WAIT, &t, sizeof(t));
}

void rsx_nir_em_fallback(rsx_nir_emitter* em, u32 dir, u32 family, u32 reason)
{
    rsx_nir_fallback f;
    memset(&f, 0, sizeof(f));
    f.dir = dir;
    f.family = family;
    f.reason = reason;
    push_action(em, RSX_NIR_OP_FALLBACK, &f, sizeof(f));
}

void rsx_nir_em_present(rsx_nir_emitter* em, u32 buffer)
{
    rsx_nir_present p;
    memset(&p, 0, sizeof(p));
    p.buffer = buffer;
    push_action(em, RSX_NIR_OP_PRESENT, &p, sizeof(p));
}
