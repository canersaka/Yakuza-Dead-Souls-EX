/*
 * ps3recomp - native-render backend core. See rsx_nr_backend.h.
 */

#include "rsx_nr_backend.h"

#include <string.h>

static void backend_flush(rsx_nr_exec_ops* x, rsx_nr_flush_reason reason)
{
    if (x->flush_reason)
        x->flush_reason(x->user, (u32)reason);
    else if (x->flush)
        x->flush(x->user);
}

void rsx_nr_backend_init(rsx_nr_backend* be, rsx_nr_ring* ring,
                         rsx_nr_tokens* tokens, const rsx_nr_exec_ops* ops)
{
    memset(be, 0, sizeof(*be));
    be->ring = ring;
    be->tokens = tokens;
    if (ops)
        be->ops = *ops;
    rsx_nir_pipeline_init(&be->st);
}

static const u32* backend_side_ptr(const u32* side, u32 side_count,
                                   u32 offset, u32 count)
{
    if (!count)
        return NULL;
    if (!side || offset > side_count || count > side_count - offset)
        return NULL;
    return side + offset;
}

static int apply_state_op(rsx_nr_backend* be, const rsx_nir_op* op,
                          const u32* side, u32 side_count)
{
    rsx_nir_pipeline* p = &be->st;
    switch (op->kind) {
    case RSX_NIR_OP_SET_SURFACE:          p->surface = op->u.surface; break;
    case RSX_NIR_OP_SET_VIEWPORT:         p->viewport = op->u.viewport; break;
    case RSX_NIR_OP_SET_SCISSOR:          p->scissor = op->u.scissor; break;
    case RSX_NIR_OP_SET_RASTER:           p->raster = op->u.raster; break;
    case RSX_NIR_OP_SET_DEPTH_STENCIL:    p->depth_stencil = op->u.depth_stencil; break;
    case RSX_NIR_OP_SET_BLEND:            p->blend = op->u.blend; break;
    case RSX_NIR_OP_SET_RENDER_CONDITION: p->render_condition = op->u.render_condition; break;
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
    case RSX_NIR_OP_SET_VERTEX_PROGRAM: {
        p->vertex_program = op->u.vertex_program;
        u32 n = op->u.vertex_program.word_count;
        if (n > RSX_NIR_VP_MAX_WORDS)
            n = RSX_NIR_VP_MAX_WORDS;
        if (n) {
            const u32* w = backend_side_ptr(
                side, side_count, op->u.vertex_program.words_ofs, n);
            if (!w)
                return -1;
            memcpy(be->vp_words, w, (size_t)n * 4);
        }
        be->vp_word_count = n;
        break;
    }
    case RSX_NIR_OP_SET_CONSTANTS: {
        const rsx_nir_constants* c = &op->u.constants;
        const u32 count = c->slot_count * 4u;
        const u32* w = backend_side_ptr(
            side, side_count, c->words_ofs, count);
        if (count && !w)
            return -1;
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
    return 0;
}

static rsx_nr_step_result backend_execute_op(
    rsx_nr_backend* be, const rsx_nir_op* op,
    const u32* side, u32 side_count)
{
    rsx_nr_exec_ops* x = &be->ops;
    int rc = 0;

    switch (op->kind) {
    case RSX_NIR_OP_TOKEN_WAIT:
        if (!rsx_nr_tokens_satisfied(be->tokens, op->u.token.token,
                                     op->u.token.value)) {
            be->stats.blocked_token++;
            return RSX_NR_STEP_BLOCKED_TOKEN;
        }
        break;
    case RSX_NIR_OP_SEMAPHORE_ACQUIRE: {
        u32 cur = 0;
        if (x->sem_read &&
            x->sem_read(x->user, op->u.semaphore.dma_context,
                        op->u.semaphore.offset, &cur) == 0 &&
            cur != op->u.semaphore.value) {
            be->stats.blocked_semaphore++;
            return RSX_NR_STEP_BLOCKED_SEMAPHORE;
        }
        break;
    }
    case RSX_NIR_OP_TOKEN_SIGNAL:
        rsx_nr_tokens_signal(be->tokens, op->u.token.token,
                             op->u.token.value);
        break;
    case RSX_NIR_OP_SEMAPHORE_RELEASE: {
        /* The back-end (0x1D70) store swizzles bytes 0<->2 on real
         * hardware — the SDK's SetWriteBackEndLabel pre-swaps its value
         * to compensate, and the live FIFO consumer applies the same
         * swizzle at store time (import_overrides.cpp 0x1D70). The
         * texture-pipe (0x1D74) store is verbatim. Apply the hardware
         * store transform HERE so sem_write callbacks always receive the
         * final memory value (SDK SetWriteBackEndLabel "swap byte 0 and
         * 2" vs SetWriteTextureLabel). */
        u32 v = op->u.semaphore.value;
        if (op->u.semaphore.texture_read == 0u)
            v = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) |
                ((v >> 16) & 0xFFu);
        /* Both back-end and texture-pipe releases publish completion of all
         * preceding graphics work. Retire the native list before making the
         * guest-visible label store observable. */
        backend_flush(x, RSX_NR_FLUSH_SEMAPHORE);
        if (x->sem_write)
            x->sem_write(x->user, op->u.semaphore.dma_context,
                         op->u.semaphore.offset, v,
                         op->u.semaphore.texture_read);
        break;
    }
    case RSX_NIR_OP_CLEAR:
        if (x->clear)
            rc = x->clear(x->user, &be->st, &op->u.clear);
        break;
    case RSX_NIR_OP_DRAW:
        if (x->draw) {
            const u32* batches = backend_side_ptr(
                side, side_count, op->u.draw.batches_ofs,
                op->u.draw.batch_count * 2u);
            if (op->u.draw.batch_count && !batches) {
                rc = -1;
                break;
            }
            rc = x->draw(x->user, &be->st, be->vp_words, be->vp_word_count,
                         &op->u.draw, batches);
        }
        break;
    case RSX_NIR_OP_TRANSFER:
        if (x->transfer) {
            const u32* words = backend_side_ptr(
                side, side_count, op->u.transfer.words_ofs,
                op->u.transfer.word_count);
            if (op->u.transfer.word_count && !words) {
                rc = -1;
                break;
            }
            rc = x->transfer(x->user, &be->st, &op->u.transfer,
                             words);
        }
        break;
    case RSX_NIR_OP_PRESENT:
        if (x->present)
            rc = x->present(x->user, op->u.present.buffer);
        break;
    case RSX_NIR_OP_BARRIER:
        backend_flush(x, RSX_NR_FLUSH_BARRIER);
        break;
    case RSX_NIR_OP_SET_REFERENCE:
        backend_flush(x, RSX_NR_FLUSH_REFERENCE);
        if (x->set_reference)
            x->set_reference(x->user, op->u.reference.value);
        break;
    case RSX_NIR_OP_REPORT:
        if (!x->report_defer ||
            x->report_defer(x->user, op->u.report.kind, op->u.report.arg,
                            op->u.report.dma_report) != 0) {
            backend_flush(x, RSX_NR_FLUSH_REPORT);
            if (x->report)
                rc = x->report(x->user, op->u.report.kind, op->u.report.arg,
                               op->u.report.dma_report);
        }
        break;
    case RSX_NIR_OP_USER_COMMAND:
        if (x->user_command)
            x->user_command(x->user, op->u.user_command.cause);
        break;
    case RSX_NIR_OP_FALLBACK:
        if (op->u.fallback.dir == RSX_NIR_FALLBACK_ENTER)
            be->stats.fallback_enters++;
        else
            be->stats.fallback_exits++;
        break;
    default:
        rc = apply_state_op(be, op, side, side_count);
        break;
    }

    if (op->kind < RSX_NIR_OP_KIND_COUNT)
        be->stats.executed[op->kind]++;
    if (rc != 0)
        be->stats.exec_errors++;
    return RSX_NR_STEP_EXECUTED;
}

rsx_nr_step_result rsx_nr_backend_step(rsx_nr_backend* be)
{
    const rsx_nr_slot* slot = rsx_nr_ring_peek(be->ring);
    if (!slot)
        return RSX_NR_STEP_EMPTY;
    const rsx_nr_step_result result = backend_execute_op(
        be, &slot->op, be->ring->side, be->ring->side_cap);
    if (result == RSX_NR_STEP_EXECUTED)
        rsx_nr_ring_pop(be->ring);
    return result;
}

rsx_nr_step_result rsx_nr_backend_stream_step(
    rsx_nr_backend* be, const rsx_nir_stream* stream, u32 index)
{
    if (!be || !stream || index >= stream->op_count)
        return RSX_NR_STEP_EMPTY;
    return backend_execute_op(
        be, &stream->ops[index], stream->side, stream->side_count);
}

u32 rsx_nr_backend_run(rsx_nr_backend* be, u32 max_ops)
{
    u32 n = 0;
    while (!max_ops || n < max_ops) {
        if (rsx_nr_backend_step(be) != RSX_NR_STEP_EXECUTED)
            break;
        n++;
    }
    return n;
}
