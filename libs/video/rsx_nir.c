/*
 * ps3recomp - Native Render IR (NIR): stream, normalization fold, and
 * equivalence comparison. See rsx_nir.h for the design.
 */

#include "rsx_nir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stream ----------------------------------------------------------- */

void rsx_nir_stream_init(rsx_nir_stream* s)
{
    memset(s, 0, sizeof(*s));
}

void rsx_nir_stream_init_fixed(rsx_nir_stream* s,
                               rsx_nir_op* ops, u32 op_cap,
                               u32* side, u32 side_cap)
{
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->op_cap = op_cap;
    s->side = side;
    s->side_cap = side_cap;
    s->fixed = 1;
}

void rsx_nir_stream_free(rsx_nir_stream* s)
{
    if (!s->fixed) {
        free(s->ops);
        free(s->side);
    }
    memset(s, 0, sizeof(*s));
}

void rsx_nir_stream_reset(rsx_nir_stream* s)
{
    s->op_count = 0;
    s->side_count = 0;
    s->oom = 0;
    s->overflow = 0;
}

static int grow(void** buf, u32* cap, u32 need, u32 elem_size)
{
    if (need <= *cap)
        return 0;
    u32 ncap = *cap ? *cap : 64;
    while (ncap < need)
        ncap *= 2;
    void* nbuf = realloc(*buf, (size_t)ncap * elem_size);
    if (!nbuf)
        return -1;
    *buf = nbuf;
    *cap = ncap;
    return 0;
}

int rsx_nir_push(rsx_nir_stream* s, const rsx_nir_op* op)
{
    if (s->oom || s->overflow)
        return -1;
    if (s->fixed) {
        if (s->op_count >= s->op_cap) {
            s->overflow = 1;
            return -1;
        }
    } else if (grow((void**)&s->ops, &s->op_cap, s->op_count + 1, sizeof(*op))) {
        s->oom = 1;
        return -1;
    }
    s->ops[s->op_count++] = *op;
    return 0;
}

u32 rsx_nir_side_push(rsx_nir_stream* s, const u32* words, u32 count)
{
    if (s->oom || s->overflow)
        return ~0u;
    if (s->fixed) {
        if (count > s->side_cap - s->side_count) {
            s->overflow = 1;
            return ~0u;
        }
    } else if (grow((void**)&s->side, &s->side_cap, s->side_count + count, 4)) {
        s->oom = 1;
        return ~0u;
    }
    u32 ofs = s->side_count;
    if (count)
        memcpy(s->side + ofs, words, (size_t)count * 4);
    s->side_count += count;
    return ofs;
}

const u32* rsx_nir_side(const rsx_nir_stream* s, u32 ofs, u32 count)
{
    if (ofs > s->side_count || count > s->side_count - ofs)
        return NULL;
    return s->side + ofs;
}

/* ---- sink ------------------------------------------------------------- */

static int stream_sink_push(void* user, const rsx_nir_op* op)
{
    return rsx_nir_push((rsx_nir_stream*)user, op);
}

static u32 stream_sink_side_push(void* user, const u32* words, u32 count)
{
    return rsx_nir_side_push((rsx_nir_stream*)user, words, count);
}

rsx_nir_sink rsx_nir_stream_sink(rsx_nir_stream* s)
{
    rsx_nir_sink k;
    k.user = s;
    k.push = stream_sink_push;
    k.side_push = stream_sink_side_push;
    return k;
}

u64 rsx_nir_hash_words(const u32* words, u32 count)
{
    /* An absent program (count 0) hashes to 0, matching the zero-initialized
     * pipeline, so "no program on either path" is equivalence, not a diff. */
    if (!count)
        return 0;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < count; i++) {
        h ^= words[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* ---- fold ------------------------------------------------------------- */

void rsx_nir_pipeline_init(rsx_nir_pipeline* p)
{
    memset(p, 0, sizeof(*p));
    /* NV40 reset defaults: both stencil masks initially enable all bits.
     * Keep packet-decoded and directly typed streams bit-equivalent before
     * either path sees an explicit stencil-state write. */
    p->depth_stencil.stencil_mask = 0xFFu;
    p->depth_stencil.stencil_write_mask = 0xFFu;
    p->depth_stencil.back_stencil_mask = 0xFFu;
    p->depth_stencil.back_stencil_write_mask = 0xFFu;
    p->depth_stencil.depth_bounds_max = 0x3F800000u;
    p->fragment_program.shader_window = 0x1000u;
    /* Hardware-default vertex attribute value (0,0,0,1), matching
     * rsx_dsp_vertex_default's never-written fallback. */
    for (u32 i = 0; i < RSX_NIR_NUM_VERTEX_ATTR; i++)
        p->vertex_bindings.attr[i].def[3] = 1.0f;
}

int rsx_nir_op_is_action(u32 kind)
{
    switch (kind) {
    case RSX_NIR_OP_CLEAR:
    case RSX_NIR_OP_DRAW:
    case RSX_NIR_OP_TRANSFER:
    case RSX_NIR_OP_BARRIER:
    case RSX_NIR_OP_SEMAPHORE_RELEASE:
    case RSX_NIR_OP_SEMAPHORE_ACQUIRE:
    case RSX_NIR_OP_REPORT:
    case RSX_NIR_OP_SET_REFERENCE:
    case RSX_NIR_OP_USER_COMMAND:
    case RSX_NIR_OP_TOKEN_SIGNAL:
    case RSX_NIR_OP_TOKEN_WAIT:
    case RSX_NIR_OP_FALLBACK:
    case RSX_NIR_OP_PRESENT:
        return 1;
    default:
        return 0;
    }
}

static void apply_state(rsx_nir_pipeline* p, const rsx_nir_stream* s,
                        const rsx_nir_op* op)
{
    switch (op->kind) {
    case RSX_NIR_OP_SET_SURFACE:          p->surface = op->u.surface; break;
    case RSX_NIR_OP_SET_VIEWPORT:         p->viewport = op->u.viewport; break;
    case RSX_NIR_OP_SET_SCISSOR:          p->scissor = op->u.scissor; break;
    case RSX_NIR_OP_SET_RASTER:           p->raster = op->u.raster; break;
    case RSX_NIR_OP_SET_DEPTH_STENCIL:    p->depth_stencil = op->u.depth_stencil; break;
    case RSX_NIR_OP_SET_BLEND:            p->blend = op->u.blend; break;
    case RSX_NIR_OP_SET_RENDER_CONDITION: p->render_condition = op->u.render_condition; break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM:   p->vertex_program = op->u.vertex_program; break;
    case RSX_NIR_OP_SET_FRAGMENT_PROGRAM: p->fragment_program = op->u.fragment_program; break;
    case RSX_NIR_OP_SET_VERTEX_BINDINGS:  p->vertex_bindings = op->u.vertex_bindings; break;
    case RSX_NIR_OP_SET_INDEX_BINDING:    p->index_binding = op->u.index_binding; break;
    case RSX_NIR_OP_SET_TEXTURE:
        if (op->unit < RSX_NIR_NUM_TEXTURES)
            p->textures[op->unit] = op->u.texture;
        break;
    case RSX_NIR_OP_SET_VERTEX_TEXTURE:
        if (op->unit < RSX_NIR_NUM_VERTEX_TEXTURES)
            p->vertex_textures[op->unit] = op->u.texture;
        break;
    case RSX_NIR_OP_SET_CONSTANTS: {
        const rsx_nir_constants* c = &op->u.constants;
        const u32* w = rsx_nir_side(s, c->words_ofs, c->slot_count * 4);
        if (!w)
            break;
        for (u32 i = 0; i < c->slot_count; i++) {
            u32 slot = c->first_slot + i;
            if (slot >= RSX_NIR_NUM_CONSTANTS)
                break;
            memcpy(p->constants[slot], w + i * 4, 16);
            p->constants_written[slot] = 1;
        }
        break;
    }
    default:
        break;
    }
}

void rsx_nir_pipeline_apply_op(rsx_nir_pipeline* p,
                               const rsx_nir_stream* stream,
                               const rsx_nir_op* op)
{
    if (p && stream && op && !rsx_nir_op_is_action(op->kind))
        apply_state(p, stream, op);
}

void rsx_nir_cursor_init(rsx_nir_cursor* c, const rsx_nir_stream* s)
{
    c->s = s;
    c->pos = 0;
    rsx_nir_pipeline_init(&c->state);
}

int rsx_nir_cursor_next(rsx_nir_cursor* c, rsx_nir_action* act)
{
    const rsx_nir_stream* s = c->s;
    while (c->pos < s->op_count) {
        const rsx_nir_op* op = &s->ops[c->pos];
        u32 idx = c->pos++;
        if (!rsx_nir_op_is_action(op->kind)) {
            apply_state(&c->state, s, op);
            continue;
        }
        act->kind = op->kind;
        act->op_index = idx;
        switch (op->kind) {
        case RSX_NIR_OP_CLEAR:             act->u.clear = op->u.clear; break;
        case RSX_NIR_OP_DRAW:              act->u.draw = op->u.draw; break;
        case RSX_NIR_OP_TRANSFER:          act->u.transfer = op->u.transfer; break;
        case RSX_NIR_OP_SEMAPHORE_RELEASE:
        case RSX_NIR_OP_SEMAPHORE_ACQUIRE: act->u.semaphore = op->u.semaphore; break;
        case RSX_NIR_OP_REPORT:            act->u.report = op->u.report; break;
        case RSX_NIR_OP_BARRIER:           act->u.barrier = op->u.barrier; break;
        case RSX_NIR_OP_SET_REFERENCE:     act->u.reference = op->u.reference; break;
        case RSX_NIR_OP_USER_COMMAND:      act->u.user_command = op->u.user_command; break;
        case RSX_NIR_OP_TOKEN_SIGNAL:
        case RSX_NIR_OP_TOKEN_WAIT:        act->u.token = op->u.token; break;
        case RSX_NIR_OP_FALLBACK:          act->u.fallback = op->u.fallback; break;
        case RSX_NIR_OP_PRESENT:           act->u.present = op->u.present; break;
        default:                           break;
        }
        act->state = c->state;
        return 1;
    }
    return 0;
}

int rsx_nir_fold(const rsx_nir_stream* s, rsx_nir_action** out)
{
    u32 n_actions = 0;
    for (u32 i = 0; i < s->op_count; i++)
        if (rsx_nir_op_is_action(s->ops[i].kind))
            n_actions++;

    rsx_nir_action* acts = NULL;
    if (n_actions) {
        acts = calloc(n_actions, sizeof(*acts));
        if (!acts)
            return -1;
    }

    rsx_nir_cursor c;
    rsx_nir_cursor_init(&c, s);
    u32 a = 0;
    while (a < n_actions && rsx_nir_cursor_next(&c, &acts[a]))
        a++;

    *out = acts;
    return (int)n_actions;
}

/* ---- compare ---------------------------------------------------------- */

#define FAIL(...) do { \
        if (errbuf && errbuf_size) \
            snprintf(errbuf, errbuf_size, __VA_ARGS__); \
        goto fail; \
    } while (0)

/* memcmp is safe on these structs only because every emitter goes through
 * value-copies of fully memset-initialized structs (no padding garbage):
 * rsx_nir_pipeline_init/apply_state copy whole structs, and both emitters
 * (adapter + native producer) build ops with memset first. */
static int cmp_block(const void* a, const void* b, size_t n)
{
    return memcmp(a, b, n);
}

static const char* action_name(u32 kind)
{
    switch (kind) {
    case RSX_NIR_OP_CLEAR:             return "CLEAR";
    case RSX_NIR_OP_DRAW:              return "DRAW";
    case RSX_NIR_OP_TRANSFER:          return "TRANSFER";
    case RSX_NIR_OP_BARRIER:           return "BARRIER";
    case RSX_NIR_OP_SEMAPHORE_RELEASE: return "SEM_RELEASE";
    case RSX_NIR_OP_SEMAPHORE_ACQUIRE: return "SEM_ACQUIRE";
    case RSX_NIR_OP_REPORT:            return "REPORT";
    case RSX_NIR_OP_SET_REFERENCE:     return "SET_REFERENCE";
    case RSX_NIR_OP_USER_COMMAND:      return "USER_COMMAND";
    case RSX_NIR_OP_TOKEN_SIGNAL:      return "TOKEN_SIGNAL";
    case RSX_NIR_OP_TOKEN_WAIT:        return "TOKEN_WAIT";
    case RSX_NIR_OP_FALLBACK:          return "FALLBACK";
    case RSX_NIR_OP_PRESENT:           return "PRESENT";
    default:                           return "?";
    }
}

/* Compare one pipeline field-group at a time so a divergence names the
 * group rather than a byte offset. */
static const char* pipeline_diverges(const rsx_nir_pipeline* x,
                                     const rsx_nir_pipeline* y)
{
    if (cmp_block(&x->surface, &y->surface, sizeof(x->surface)))            return "surface";
    if (cmp_block(&x->viewport, &y->viewport, sizeof(x->viewport)))         return "viewport";
    if (cmp_block(&x->scissor, &y->scissor, sizeof(x->scissor)))            return "scissor";
    if (cmp_block(&x->raster, &y->raster, sizeof(x->raster)))               return "raster";
    if (cmp_block(&x->depth_stencil, &y->depth_stencil,
                  sizeof(x->depth_stencil)))                                return "depth_stencil";
    if (cmp_block(&x->blend, &y->blend, sizeof(x->blend)))                  return "blend";
    if (cmp_block(&x->render_condition, &y->render_condition,
                  sizeof(x->render_condition)))                            return "render_condition";
    /* vertex program: compare identity (hash/count/slot/masks), not the
     * side-buffer offset, which differs between streams by construction */
    if (x->vertex_program.start_slot != y->vertex_program.start_slot ||
        x->vertex_program.word_count != y->vertex_program.word_count ||
        x->vertex_program.hash != y->vertex_program.hash ||
        x->vertex_program.attrib_input_mask != y->vertex_program.attrib_input_mask ||
        x->vertex_program.attrib_output_mask != y->vertex_program.attrib_output_mask ||
        x->vertex_program.branch_bits != y->vertex_program.branch_bits)
        return "vertex_program";
    if (cmp_block(&x->fragment_program, &y->fragment_program,
                  sizeof(x->fragment_program)))                             return "fragment_program";
    if (cmp_block(&x->vertex_bindings, &y->vertex_bindings,
                  sizeof(x->vertex_bindings)))                              return "vertex_bindings";
    if (cmp_block(&x->index_binding, &y->index_binding,
                  sizeof(x->index_binding)))                                return "index_binding";
    if (cmp_block(x->textures, y->textures, sizeof(x->textures)))           return "textures";
    if (cmp_block(x->vertex_textures, y->vertex_textures,
                  sizeof(x->vertex_textures)))                              return "vertex_textures";
    if (cmp_block(x->constants, y->constants, sizeof(x->constants)))        return "constants";
    if (cmp_block(x->constants_written, y->constants_written,
                  sizeof(x->constants_written)))                            return "constants_written";
    return NULL;
}

int rsx_nir_compare(const rsx_nir_stream* a, const rsx_nir_stream* b,
                    char* errbuf, unsigned errbuf_size)
{
    return rsx_nir_compare_ex(a, b, 0, errbuf, errbuf_size);
}

/* cursor_next that skips masked action kinds */
static int next_unmasked(rsx_nir_cursor* c, rsx_nir_action* act, u32 mask)
{
    for (;;) {
        int h = rsx_nir_cursor_next(c, act);
        if (!h)
            return 0;
        if (act->kind < 32 && (mask & (1u << act->kind)))
            continue;
        return 1;
    }
}

int rsx_nir_compare_ex(const rsx_nir_stream* a, const rsx_nir_stream* b,
                       u32 skip_kind_mask, char* errbuf, unsigned errbuf_size)
{
    /* Two streaming cursors: memory stays bounded on capture-sized input. */
    rsx_nir_action* fa = malloc(sizeof(*fa));
    rsx_nir_action* fb = malloc(sizeof(*fb));
    rsx_nir_cursor ca, cb;
    rsx_nir_cursor_init(&ca, a);
    rsx_nir_cursor_init(&cb, b);
    if (!fa || !fb)
        FAIL("compare: allocation failure");

    for (int i = 0;; i++) {
        int ha = next_unmasked(&ca, fa, skip_kind_mask);
        int hb = next_unmasked(&cb, fb, skip_kind_mask);
        if (ha != hb)
            FAIL("action count differs at %d: stream %s ended first", i,
                 ha ? "B" : "A");
        if (!ha)
            break;

        const rsx_nir_action* x = fa;
        const rsx_nir_action* y = fb;
        if (x->kind != y->kind)
            FAIL("action %d: kind %s vs %s", i,
                 action_name(x->kind), action_name(y->kind));

        switch (x->kind) {
        case RSX_NIR_OP_CLEAR:
            if (cmp_block(&x->u.clear, &y->u.clear, sizeof(x->u.clear)))
                FAIL("action %d (CLEAR): payload differs "
                     "(mask %08X/%08X color %08X/%08X)", i,
                     x->u.clear.mask, y->u.clear.mask,
                     x->u.clear.color_value, y->u.clear.color_value);
            break;
        case RSX_NIR_OP_DRAW: {
            const rsx_nir_draw* dx = &x->u.draw;
            const rsx_nir_draw* dy = &y->u.draw;
            if (dx->primitive != dy->primitive || dx->indexed != dy->indexed ||
                dx->batch_count != dy->batch_count ||
                dx->total_count != dy->total_count)
                FAIL("action %d (DRAW): shape differs "
                     "(prim %u/%u idx %u/%u batches %u/%u total %u/%u)", i,
                     dx->primitive, dy->primitive, dx->indexed, dy->indexed,
                     dx->batch_count, dy->batch_count,
                     dx->total_count, dy->total_count);
            const u32* bx = rsx_nir_side(a, dx->batches_ofs, dx->batch_count * 2);
            const u32* by = rsx_nir_side(b, dy->batches_ofs, dy->batch_count * 2);
            if ((dx->batch_count && (!bx || !by)) ||
                (bx && by && memcmp(bx, by, (size_t)dx->batch_count * 8)))
                FAIL("action %d (DRAW): batch list differs", i);
            break;
        }
        case RSX_NIR_OP_TRANSFER: {
            const rsx_nir_transfer* tx = &x->u.transfer;
            const rsx_nir_transfer* ty = &y->u.transfer;
            /* compare all fields except the side-buffer offset, which
             * differs between streams by construction */
            rsx_nir_transfer nx = *tx, ny = *ty;
            nx.words_ofs = ny.words_ofs = 0;
            if (cmp_block(&nx, &ny, sizeof(nx)))
                FAIL("action %d (TRANSFER): payload differs "
                     "(kind %u/%u dst %08X/%08X)", i, tx->kind, ty->kind,
                     tx->dst_offset, ty->dst_offset);
            const u32* wx = rsx_nir_side(a, tx->words_ofs, tx->word_count);
            const u32* wy = rsx_nir_side(b, ty->words_ofs, ty->word_count);
            if ((tx->word_count && (!wx || !wy)) ||
                (wx && wy && memcmp(wx, wy, (size_t)tx->word_count * 4)))
                FAIL("action %d (TRANSFER): inline payload differs", i);
            break;
        }
        case RSX_NIR_OP_SEMAPHORE_RELEASE:
        case RSX_NIR_OP_SEMAPHORE_ACQUIRE:
            if (cmp_block(&x->u.semaphore, &y->u.semaphore, sizeof(x->u.semaphore)))
                FAIL("action %d (%s): payload differs (ofs %08X/%08X val %08X/%08X)",
                     i, action_name(x->kind),
                     x->u.semaphore.offset, y->u.semaphore.offset,
                     x->u.semaphore.value, y->u.semaphore.value);
            break;
        case RSX_NIR_OP_REPORT:
            if (cmp_block(&x->u.report, &y->u.report, sizeof(x->u.report)))
                FAIL("action %d (REPORT): payload differs", i);
            break;
        case RSX_NIR_OP_BARRIER:
            if (cmp_block(&x->u.barrier, &y->u.barrier, sizeof(x->u.barrier)))
                FAIL("action %d (BARRIER): payload differs", i);
            break;
        case RSX_NIR_OP_SET_REFERENCE:
            if (cmp_block(&x->u.reference, &y->u.reference, sizeof(x->u.reference)))
                FAIL("action %d (SET_REFERENCE): value %08X vs %08X", i,
                     x->u.reference.value, y->u.reference.value);
            break;
        case RSX_NIR_OP_USER_COMMAND:
            if (cmp_block(&x->u.user_command, &y->u.user_command,
                          sizeof(x->u.user_command)))
                FAIL("action %d (USER_COMMAND): cause %08X vs %08X", i,
                     x->u.user_command.cause, y->u.user_command.cause);
            break;
        case RSX_NIR_OP_TOKEN_SIGNAL:
        case RSX_NIR_OP_TOKEN_WAIT:
            if (cmp_block(&x->u.token, &y->u.token, sizeof(x->u.token)))
                FAIL("action %d (%s): token %u=%u vs %u=%u", i,
                     action_name(x->kind),
                     x->u.token.token, x->u.token.value,
                     y->u.token.token, y->u.token.value);
            break;
        case RSX_NIR_OP_FALLBACK:
            if (cmp_block(&x->u.fallback, &y->u.fallback, sizeof(x->u.fallback)))
                FAIL("action %d (FALLBACK): dir/family/reason %u/%u/%u vs %u/%u/%u",
                     i, x->u.fallback.dir, x->u.fallback.family,
                     x->u.fallback.reason, y->u.fallback.dir,
                     y->u.fallback.family, y->u.fallback.reason);
            break;
        case RSX_NIR_OP_PRESENT:
            if (cmp_block(&x->u.present, &y->u.present, sizeof(x->u.present)))
                FAIL("action %d (PRESENT): buffer %u vs %u", i,
                     x->u.present.buffer, y->u.present.buffer);
            break;
        default:
            break;
        }

        const char* group = pipeline_diverges(&x->state, &y->state);
        if (group)
            FAIL("action %d (%s): pipeline state group '%s' differs "
                 "(stream A op %u, stream B op %u)", i,
                 action_name(x->kind), group, x->op_index, y->op_index);

        /* vertex-program words: identity hash matched above; verify the
         * side-buffer content actually backs the hash on both sides */
        if (x->state.vertex_program.word_count) {
            const rsx_nir_vertex_program* px = &x->state.vertex_program;
            const rsx_nir_vertex_program* py = &y->state.vertex_program;
            const u32* wx = rsx_nir_side(a, px->words_ofs, px->word_count);
            const u32* wy = rsx_nir_side(b, py->words_ofs, py->word_count);
            if (!wx || !wy)
                FAIL("action %d: vertex program words out of range", i);
            if (memcmp(wx, wy, (size_t)px->word_count * 4))
                FAIL("action %d: vertex program words differ (hash collision?)", i);
        }
    }

    free(fa);
    free(fb);
    return 0;

fail:
    free(fa);
    free(fb);
    return -1;
}
