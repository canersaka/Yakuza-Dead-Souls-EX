/*
 * ps3recomp - NIR emitter: the canonical state-diffing front end shared by
 * every NIR producer.
 *
 * Producers stage state through typed setters; when an ACTION is emitted
 * (clear/draw/transfer/present/semaphore/report/reference/user-command/
 * token/fallback), the emitter first appends SET ops for exactly the state
 * groups that changed since they were last emitted, then the action op.
 * Because the packet-decode adapter (rsx_nir_adapter) and the native
 * interception path both route through this one component, two producers
 * that stage identical state and actions yield identical folded streams —
 * that is the equivalence contract.
 *
 * Output goes through an rsx_nir_sink, so the same emitter serves the
 * offline stream (equivalence harness) and the live submission ring with
 * no intermediate copy. The emitter itself performs no allocation.
 *
 * Constants are staged per-slot and emitted as contiguous dirty runs.
 * Vertex-program words are staged in full and emitted when their identity
 * (hash/count/start/masks) changes.
 */

#ifndef PS3RECOMP_RSX_NIR_EMITTER_H
#define PS3RECOMP_RSX_NIR_EMITTER_H

#include "rsx_nir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_nir_emitter {
    rsx_nir_sink out;

    /* last-emitted state (what a folding consumer has seen) */
    rsx_nir_pipeline shadow;
    /* staged state (what the producer has set) */
    rsx_nir_pipeline pending;

    /* staged constant slots not yet emitted */
    u8 const_dirty[RSX_NIR_NUM_CONSTANTS];

    /* staged vertex-program words (identity lives in pending.vertex_program) */
    u32 vp_words[RSX_NIR_VP_MAX_WORDS];

    /* first action ever: emit every group once so the stream is
     * self-contained even for all-default state */
    int primed;
} rsx_nir_emitter;

void rsx_nir_emitter_init(rsx_nir_emitter* em, const rsx_nir_sink* out);
/* Convenience: emit into a stream. */
void rsx_nir_emitter_init_stream(rsx_nir_emitter* em, rsx_nir_stream* out);

/* ---- typed state setters (stage only; nothing is appended yet) -------- */

void rsx_nir_em_surface(rsx_nir_emitter* em, const rsx_nir_surface* s);
void rsx_nir_em_viewport(rsx_nir_emitter* em, const rsx_nir_viewport* v);
void rsx_nir_em_scissor(rsx_nir_emitter* em, const rsx_nir_scissor* s);
void rsx_nir_em_raster(rsx_nir_emitter* em, const rsx_nir_raster* r);
void rsx_nir_em_depth_stencil(rsx_nir_emitter* em, const rsx_nir_depth_stencil* d);
void rsx_nir_em_blend(rsx_nir_emitter* em, const rsx_nir_blend* b);
void rsx_nir_em_render_condition(rsx_nir_emitter* em,
                                 const rsx_nir_render_condition* c);
void rsx_nir_em_fragment_program(rsx_nir_emitter* em, const rsx_nir_fragment_program* f);
void rsx_nir_em_vertex_bindings(rsx_nir_emitter* em, const rsx_nir_vertex_bindings* v);
void rsx_nir_em_index_binding(rsx_nir_emitter* em, const rsx_nir_index_binding* i);
void rsx_nir_em_texture(rsx_nir_emitter* em, u32 unit, const rsx_nir_texture* t);
void rsx_nir_em_vertex_texture(rsx_nir_emitter* em, u32 unit, const rsx_nir_texture* t);

/* words = 4 u32 (f32 bit patterns) per slot */
void rsx_nir_em_constants(rsx_nir_emitter* em, u32 first_slot, u32 slot_count,
                          const u32* words);

void rsx_nir_em_vertex_program(rsx_nir_emitter* em, u32 start_slot,
                               const u32* words, u32 word_count,
                               u32 attrib_input_mask, u32 attrib_output_mask,
                               u32 branch_bits);

/* ---- actions (flush dirty state, then append the action op) ----------- */

void rsx_nir_em_clear(rsx_nir_emitter* em, u32 mask, u32 color_value,
                      u32 depth_value, u32 stencil_value);
/* batches = {first,count} u32 pairs */
void rsx_nir_em_draw(rsx_nir_emitter* em, u32 primitive, u32 indexed,
                     const u32* batches, u32 batch_count);
/* t->word_count inline words are copied from `words` (NULL when zero);
 * t->words_ofs is assigned here. */
void rsx_nir_em_transfer(rsx_nir_emitter* em, const rsx_nir_transfer* t,
                         const u32* words);
void rsx_nir_em_semaphore_release(rsx_nir_emitter* em, u32 dma_context,
                                  u32 offset, u32 value, u32 texture_read);
void rsx_nir_em_semaphore_acquire(rsx_nir_emitter* em, u32 dma_context,
                                  u32 offset, u32 value);
void rsx_nir_em_report(rsx_nir_emitter* em, u32 kind, u32 arg, u32 dma_report);
void rsx_nir_em_barrier(rsx_nir_emitter* em, u32 kind);
void rsx_nir_em_set_reference(rsx_nir_emitter* em, u32 value);
void rsx_nir_em_user_command(rsx_nir_emitter* em, u32 cause);
void rsx_nir_em_token_signal(rsx_nir_emitter* em, u32 token, u32 value);
void rsx_nir_em_token_wait(rsx_nir_emitter* em, u32 token, u32 value);
void rsx_nir_em_fallback(rsx_nir_emitter* em, u32 dir, u32 family, u32 reason);
void rsx_nir_em_present(rsx_nir_emitter* em, u32 buffer);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NIR_EMITTER_H */
