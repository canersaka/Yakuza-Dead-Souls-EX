/*
 * ps3recomp - native-render producer interception layer. See
 * rsx_nr_intercept.h for the contract.
 */

#include "rsx_nr_intercept.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Worst-case ops one command can flush: 9 state groups + 16 fragment + 4
 * vertex textures + 1 vertex program + constant runs (worst interleave
 * bounded by the emitter's contiguous-run merge; 32 is generous) + the
 * action + 2 ordering markers. */
#define NR_CMD_MAX_OPS  96u
/* Worst-case state side words: full VP (544*4) + all constants (512*4). */
#define NR_STATE_SIDE   (RSX_NIR_VP_MAX_WORDS + RSX_NIR_NUM_CONSTANTS * 4u)

u32 rsx_nr_parse_families(const char* spec)
{
    if (!spec || !spec[0] || (spec[0] == '0' && !spec[1]))
        return 0;
    if ((spec[0] == '1' && !spec[1]) || strcmp(spec, "all") == 0)
        return (1u << RSX_NR_FAM_COUNT) - 1u;

    static const char* names[RSX_NR_FAM_COUNT] = {
        "flip", "clear", "state", "draw", "program",
        "transfer", "semaphore", "report", "user", "sync",
    };
    u32 mask = 0;
    const char* p = spec;
    while (*p) {
        const char* e = p;
        while (*e && *e != ',')
            e++;
        size_t n = (size_t)(e - p);
        for (u32 f = 0; f < RSX_NR_FAM_COUNT; f++) {
            if (n == strlen(names[f]) && memcmp(p, names[f], n) == 0) {
                mask |= 1u << f;
                break;
            }
        }
        p = *e ? e + 1 : e;
    }
    return mask;
}

int rsx_nr_intercept_init(rsx_nr_intercept* it, rsx_nr_ring* ring,
                          rsx_nr_tokens* tokens, u32 families,
                          int arm_shadow)
{
    memset(it, 0, sizeof(*it));
    it->ring = ring;
    it->tokens = tokens;
    it->families = families;
    it->shadow_armed = arm_shadow ? 1 : 0;
    rsx_nir_sink k = rsx_nr_ring_sink(ring);
    rsx_nir_adapter_init_sink(&it->shadow, &k);
    it->shadow.shadow_mode = 1;
    return 0;
}

/* ---- decode-side shadow ------------------------------------------------ */

void rsx_nr_intercept_shadow_method(rsx_nr_intercept* it, u32 method, u32 arg)
{
    if (!it->shadow_armed)
        return;
    it->stats.shadow_methods++;
    rsx_nir_adapter_method(&it->shadow, method, arg);
}

/* ---- ordering ---------------------------------------------------------- */

void rsx_nr_intercept_fifo_drained(rsx_nr_intercept* it, u32 episode)
{
    rsx_nr_tokens_signal(it->tokens, RSX_NR_TOKEN_FIFO_DRAIN, episode);
}

void rsx_nr_note_fallback(rsx_nr_intercept* it, u32 fam, u32 reason)
{
    if (fam < RSX_NR_FAM_COUNT && reason < RSX_NR_FB_REASON_COUNT)
        it->stats.fallbacks[fam][reason]++;
    if (!it->families)
        return;                 /* fully disabled: pure FIFO, no consumers */
    if (it->in_fallback)
        return;                 /* already inside a FIFO-owned episode     */
    /* open a fallback episode: the ring's consumer must not run anything
     * emitted after this point until the FIFO catches up */
    if (rsx_nr_ring_can_accept(it->ring, 1, 0)) {
        rsx_nir_op op;
        memset(&op, 0, sizeof(op));
        op.kind = RSX_NIR_OP_FALLBACK;
        op.u.fallback.dir = RSX_NIR_FALLBACK_ENTER;
        op.u.fallback.family = fam;
        op.u.fallback.reason = reason;
        rsx_nr_ring_push(it->ring, &op);
    }
    /* a full ring leaves the sticky reject set by the next push attempt;
     * the episode is still opened so ordering stays conservative */
    it->in_fallback = 1;
    it->drain_seq++;
    it->stats.fallback_episodes++;
}

/* Gate + transition for one native command. extra_ops/extra_side are the
 * action's own footprint beyond the state flush. Returns 1 when the
 * caller may emit. */
static int begin_native(rsx_nr_intercept* it, u32 fam, u32 extra_ops,
                        u32 extra_side, int needs_state)
{
    if (!rsx_nr_family_enabled(it, fam)) {
        rsx_nr_note_fallback(it, fam, RSX_NR_FB_DISABLED);
        return 0;
    }
    if (needs_state && !it->shadow_armed) {
        rsx_nr_note_fallback(it, fam, RSX_NR_FB_NO_SHADOW);
        return 0;
    }
    if (rsx_nr_ring_reject_sticky(it->ring)) {
        rsx_nr_note_fallback(it, fam, RSX_NR_FB_REJECT);
        return 0;
    }
    if (!rsx_nr_ring_can_accept(it->ring, NR_CMD_MAX_OPS + extra_ops,
                                NR_STATE_SIDE + extra_side)) {
        rsx_nr_note_fallback(it, fam, RSX_NR_FB_CAPACITY);
        return 0;
    }
    if (it->in_fallback) {
        /* re-enter native ownership: close the episode and order the
         * native consumer behind the FIFO's drain point */
        rsx_nir_op op;
        memset(&op, 0, sizeof(op));
        op.kind = RSX_NIR_OP_FALLBACK;
        op.u.fallback.dir = RSX_NIR_FALLBACK_EXIT;
        op.u.fallback.family = fam;
        rsx_nr_ring_push(it->ring, &op);
        memset(&op, 0, sizeof(op));
        op.kind = RSX_NIR_OP_TOKEN_WAIT;
        op.u.token.token = RSX_NR_TOKEN_FIFO_DRAIN;
        op.u.token.value = it->drain_seq;
        rsx_nr_ring_push(it->ring, &op);
        it->in_fallback = 0;
    }
    return 1;
}

static void finish_native(rsx_nr_intercept* it, u32 fam)
{
    if (rsx_nr_ring_reject_sticky(it->ring)) {
        /* backstop: the pre-check should make this unreachable. Count it
         * loudly in the stats, resync the emitter's shadow diff state, and
         * poison the family so later commands fall back. */
        it->stats.fallbacks[fam][RSX_NR_FB_REJECT]++;
        it->shadow.em.primed = 0;
        it->families &= ~(1u << fam);
        return;
    }
    it->stats.native_ops[fam]++;
}

/* ---- typed entry points ------------------------------------------------ */

int rsx_nr_try_flip(rsx_nr_intercept* it, u32 buffer_id,
                    int wait_for_label, u32 label_index, u32 label_value)
{
    if (buffer_id >= 8u) {
        /* packet path refuses too (CELL_GCM_ERROR_INVALID_VALUE): let the
         * caller run it for identical refusal semantics */
        rsx_nr_note_fallback(it, RSX_NR_FAM_FLIP, RSX_NR_FB_UNSUPPORTED);
        return 0;
    }
    if (!begin_native(it, RSX_NR_FAM_FLIP, 4, 0, 0))
        return 0;
    if (wait_for_label)
        rsx_nir_em_semaphore_acquire(&it->shadow.em, RSX_NR_DMA_SEMAPHORE_RW,
                                     (label_index & 0xFFu) * 0x10u,
                                     label_value);
    rsx_nir_em_present(&it->shadow.em, buffer_id);
    finish_native(it, RSX_NR_FAM_FLIP);
    return 1;
}

int rsx_nr_try_clear(rsx_nr_intercept* it, u32 mask, u32 color_value,
                     u32 depth_value, u32 stencil_value)
{
    if (!begin_native(it, RSX_NR_FAM_CLEAR, 1, 0, 1))
        return 0;
    rsx_nir_adapter_stage_state(&it->shadow);
    rsx_nir_em_clear(&it->shadow.em, mask, color_value, depth_value,
                     stencil_value);
    finish_native(it, RSX_NR_FAM_CLEAR);
    return 1;
}

int rsx_nr_try_draw(rsx_nr_intercept* it, u32 primitive, u32 indexed,
                    const u32* batches, u32 batch_count)
{
    if (!batch_count || batch_count > RSX_NIR_ADAPTER_MAX_BATCHES) {
        rsx_nr_note_fallback(it, RSX_NR_FAM_DRAW, RSX_NR_FB_UNSUPPORTED);
        return 0;
    }
    if (!begin_native(it, RSX_NR_FAM_DRAW, 1, batch_count * 2, 1))
        return 0;
    rsx_nir_adapter_stage_state(&it->shadow);
    rsx_nir_em_draw(&it->shadow.em, primitive, indexed, batches, batch_count);
    finish_native(it, RSX_NR_FAM_DRAW);
    return 1;
}

int rsx_nr_try_transfer(rsx_nr_intercept* it, const rsx_nir_transfer* t,
                        const u32* words)
{
    if (t->word_count && !words) {
        rsx_nr_note_fallback(it, RSX_NR_FAM_TRANSFER, RSX_NR_FB_UNSUPPORTED);
        return 0;
    }
    if (!begin_native(it, RSX_NR_FAM_TRANSFER, 1, t->word_count, 1))
        return 0;
    rsx_nir_adapter_stage_state(&it->shadow);
    rsx_nir_em_transfer(&it->shadow.em, t, words);
    finish_native(it, RSX_NR_FAM_TRANSFER);
    return 1;
}

int rsx_nr_try_semaphore_release(rsx_nr_intercept* it, u32 dma_context,
                                 u32 offset, u32 value, u32 texture_read)
{
    if (!begin_native(it, RSX_NR_FAM_SEMAPHORE, 1, 0, 0))
        return 0;
    rsx_nir_em_semaphore_release(&it->shadow.em, dma_context, offset, value,
                                 texture_read);
    finish_native(it, RSX_NR_FAM_SEMAPHORE);
    return 1;
}

int rsx_nr_try_semaphore_acquire(rsx_nr_intercept* it, u32 dma_context,
                                 u32 offset, u32 value)
{
    if (!begin_native(it, RSX_NR_FAM_SEMAPHORE, 1, 0, 0))
        return 0;
    rsx_nir_em_semaphore_acquire(&it->shadow.em, dma_context, offset, value);
    finish_native(it, RSX_NR_FAM_SEMAPHORE);
    return 1;
}

int rsx_nr_try_report(rsx_nr_intercept* it, u32 kind, u32 arg, u32 dma_report)
{
    if (!begin_native(it, RSX_NR_FAM_REPORT, 1, 0, 0))
        return 0;
    rsx_nir_em_report(&it->shadow.em, kind, arg, dma_report);
    finish_native(it, RSX_NR_FAM_REPORT);
    return 1;
}

int rsx_nr_try_user_command(rsx_nr_intercept* it, u32 cause)
{
    if (!begin_native(it, RSX_NR_FAM_USER, 1, 0, 0))
        return 0;
    rsx_nir_em_user_command(&it->shadow.em, cause);
    finish_native(it, RSX_NR_FAM_USER);
    return 1;
}

int rsx_nr_try_set_reference(rsx_nr_intercept* it, u32 value)
{
    if (!begin_native(it, RSX_NR_FAM_SYNC, 1, 0, 0))
        return 0;
    rsx_nir_em_set_reference(&it->shadow.em, value);
    finish_native(it, RSX_NR_FAM_SYNC);
    return 1;
}

int rsx_nr_try_barrier(rsx_nr_intercept* it, u32 kind)
{
    if (!begin_native(it, RSX_NR_FAM_SYNC, 1, 0, 0))
        return 0;
    rsx_nir_em_barrier(&it->shadow.em, kind);
    finish_native(it, RSX_NR_FAM_SYNC);
    return 1;
}

int rsx_nr_try_token_wait(rsx_nr_intercept* it, u32 token, u32 value)
{
    if (token < RSX_NR_TOKEN_FIRST_FREE || token >= RSX_NR_MAX_TOKENS) {
        rsx_nr_note_fallback(it, RSX_NR_FAM_SYNC, RSX_NR_FB_UNSUPPORTED);
        return 0;
    }
    if (!begin_native(it, RSX_NR_FAM_SYNC, 1, 0, 0))
        return 0;
    rsx_nir_em_token_wait(&it->shadow.em, token, value);
    finish_native(it, RSX_NR_FAM_SYNC);
    return 1;
}

int rsx_nr_try_token_signal(rsx_nr_intercept* it, u32 token, u32 value)
{
    if (token < RSX_NR_TOKEN_FIRST_FREE || token >= RSX_NR_MAX_TOKENS) {
        rsx_nr_note_fallback(it, RSX_NR_FAM_SYNC, RSX_NR_FB_UNSUPPORTED);
        return 0;
    }
    if (!begin_native(it, RSX_NR_FAM_SYNC, 1, 0, 0))
        return 0;
    rsx_nir_em_token_signal(&it->shadow.em, token, value);
    finish_native(it, RSX_NR_FAM_SYNC);
    return 1;
}

/* ---- counters ---------------------------------------------------------- */

void rsx_nr_intercept_get_stats(const rsx_nr_intercept* it, rsx_nr_stats* out)
{
    *out = it->stats;
}

void rsx_nr_intercept_note_equivalence_failure(rsx_nr_intercept* it)
{
    it->stats.equivalence_failures++;
}

u32 rsx_nr_stats_format(const rsx_nr_stats* st, char* buf, u32 buf_size)
{
    static const char tag[RSX_NR_FAM_COUNT] = {
        'f', 'c', 's', 'd', 'p', 't', 'm', 'r', 'u', 'y'
    };
    unsigned long long nat = 0, fb = 0, unk = 0, cap = 0;
    for (u32 f = 0; f < RSX_NR_FAM_COUNT; f++) {
        nat += st->native_ops[f];
        for (u32 r = 0; r < RSX_NR_FB_REASON_COUNT; r++)
            fb += st->fallbacks[f][r];
        unk += st->fallbacks[f][RSX_NR_FB_UNKNOWN];
        cap += st->fallbacks[f][RSX_NR_FB_CAPACITY];
    }
    u32 n = (u32)snprintf(buf, buf_size,
                          "nr: native=%llu fb=%llu (unk=%llu cap=%llu) "
                          "eps=%llu shadow=%llu eqfail=%llu |",
                          nat, fb, unk, cap, st->fallback_episodes,
                          st->shadow_methods, st->equivalence_failures);
    for (u32 f = 0; f < RSX_NR_FAM_COUNT && n < buf_size; f++) {
        if (!st->native_ops[f])
            continue;
        n += (u32)snprintf(buf + n, buf_size > n ? buf_size - n : 0,
                           " %c=%llu", tag[f], st->native_ops[f]);
    }
    return n < buf_size ? n : buf_size - 1;
}

u32 rsx_nr_shadow_census_format(const rsx_nr_intercept* it,
                                char* buf, u32 buf_size)
{
    u64 by_class[3] = {0, 0, 0};
    u32 top_reg[8] = {0};
    u32 top_count[8] = {0};
    unsigned long long native = 0, fallback = 0;

    if (!buf_size)
        return 0;

    for (u32 f = 0; f < RSX_NR_FAM_COUNT; f++) {
        native += it->stats.native_ops[f];
        for (u32 reason = 0; reason < RSX_NR_FB_REASON_COUNT; reason++)
            fallback += it->stats.fallbacks[f][reason];
    }

    for (u32 reg = 0; reg < RSX_DSP_NUM_REGS; reg++) {
        const u32 seen = it->shadow.rsx.seen[reg];
        if (!seen)
            continue;
        const u32 klass = it->shadow.rsx.klass[reg] <= RSX_DSP_CLASS_EXEC
                              ? it->shadow.rsx.klass[reg]
                              : RSX_DSP_CLASS_TODO;
        by_class[klass] += seen;
        if (klass != RSX_DSP_CLASS_TODO)
            continue;
        for (u32 slot = 0; slot < 8; slot++) {
            if (seen <= top_count[slot])
                continue;
            for (u32 move = 7; move > slot; move--) {
                top_count[move] = top_count[move - 1];
                top_reg[move] = top_reg[move - 1];
            }
            top_count[slot] = seen;
            top_reg[slot] = reg;
            break;
        }
    }

    const u64 classified = by_class[0] + by_class[1] + by_class[2];
    const u64 unclassified = it->stats.shadow_methods > classified
                                 ? it->stats.shadow_methods - classified
                                 : 0;
    u32 n = (u32)snprintf(
        buf, buf_size,
        "nr-shadow: methods=%llu state=%llu exec=%llu stored=%llu "
        "unclassified=%llu native=%llu fb=%llu eps=%llu top-stored=",
        it->stats.shadow_methods,
        (unsigned long long)by_class[RSX_DSP_CLASS_STATE],
        (unsigned long long)by_class[RSX_DSP_CLASS_EXEC],
        (unsigned long long)by_class[RSX_DSP_CLASS_TODO],
        (unsigned long long)unclassified, native, fallback,
        it->stats.fallback_episodes);
    for (u32 slot = 0; slot < 8 && top_count[slot] && n < buf_size; slot++) {
        n += (u32)snprintf(buf + n, buf_size > n ? buf_size - n : 0,
                           "%s0x%05X:%u", slot ? "," : "",
                           top_reg[slot] << 2, top_count[slot]);
    }
    return n < buf_size ? n : buf_size - 1;
}

/* ---- packet spec helper ------------------------------------------------ */

u32 rsx_nr_flip_packet_spec(u32* out, u32 buffer_id, int wait_for_label,
                            u32 label_index, u32 label_value)
{
    /* Byte-for-byte the sequence yz_gcm_append_flip_commands() writes
     * (yakuza/import_overrides.cpp), the functional match of the lifted
     * Sony flip body (func_02108948). */
    u32 n = 0;
    if (wait_for_label) {
        out[n++] = 0x00040060u;                  /* SET_CONTEXT_DMA_SEMAPHORE */
        out[n++] = RSX_NR_DMA_SEMAPHORE_RW;
        out[n++] = 0x00040064u;                  /* SEMAPHORE_OFFSET          */
        out[n++] = (label_index & 0xFFu) * 0x10u;
        out[n++] = 0x00040068u;                  /* SEMAPHORE_ACQUIRE         */
        out[n++] = label_value;
    }
    out[n++] = 0x0004E944u;                      /* queue buffer on head 1    */
    out[n++] = buffer_id;
    out[n++] = 0x0004E924u;                      /* flip head 1               */
    out[n++] = 0x8000010Fu;                      /* use queued buffer         */
    return n;
}
