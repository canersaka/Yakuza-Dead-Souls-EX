/* Producer-island/pass compiler unit suite (docs/HANA_ISLAND_COMPILER.md).
 *
 * Every behavioral test runs the same synthetic FIFO through two lanes:
 * the plain strict frame owner (the adapter/emitter oracle) and the island
 * compiler wrapping an identical owner. Equivalence is judged on the folded
 * state observed at every executed action (a pipeline-content hash) plus
 * the exact action payloads, which is the same invariant rsx_nir_compare
 * checks for stream producers.
 */

#include "rsx_nr_island.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, ...) do { if (!(c)) { \
    fprintf(stderr, "FAIL(%d): ", __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); return 1; } } while (0)

#define TEST_WORDS (0x20000u / 4u)
#define TEST_RING_OPS 1024u
#define TEST_RING_SIDE 65536u
#define TEST_LOG_MAX 4096u
#define TEST_ARENA_BYTES (1u << 20)
#define TEST_INDEX_CAP 1024u
#define TEST_SCRATCH_OPS 1024u
#define TEST_SCRATCH_SIDE 65536u

typedef struct action_log_entry {
    u32 kind;
    u32 a, b, c, d;
    u64 state_hash;
    u64 payload_hash;
} action_log_entry;

typedef struct lane {
    u32 words[TEST_WORDS];
    u32 put;
    u32 semaphore_value;        /* backing store for acquire/release       */
    u32 references;
    action_log_entry log[TEST_LOG_MAX];
    u32 log_count;
    int log_overflow;
    rsx_nr_slot slots[TEST_RING_OPS];
    u32 side[TEST_RING_SIDE];
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nir_adapter adapter;
    rsx_nr_backend backend;
    rsx_nr_frame_owner owner;
} lane;

static u64 fnv_bytes(u64 h, const void* data, size_t n)
{
    const unsigned char* p = data;
    if (!h)
        h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static u64 lane_state_hash(const lane* l)
{
    rsx_nir_pipeline st = l->backend.st;
    st.vertex_program.words_ofs = 0;    /* transport detail, not identity  */
    u64 h = fnv_bytes(0, &st, sizeof(st));
    h = fnv_bytes(h, l->backend.vp_words,
                  (size_t)l->backend.vp_word_count * 4u);
    return h;
}

static action_log_entry* lane_log_push(lane* l, u32 kind)
{
    static action_log_entry scratch;
    action_log_entry* e;
    if (l->log_count >= TEST_LOG_MAX) {
        l->log_overflow = 1;
        e = &scratch;
    } else {
        e = &l->log[l->log_count++];
    }
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->state_hash = lane_state_hash(l);
    return e;
}

static int lane_exec_clear(void* user, const rsx_nir_pipeline* st,
                           const rsx_nir_clear* c)
{
    lane* const l = user;
    (void)st;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_CLEAR);
    e->a = c->mask;
    e->b = c->color_value;
    e->c = c->depth_value;
    e->d = c->stencil_value;
    return 0;
}

static int lane_exec_draw(void* user, const rsx_nir_pipeline* st,
                          const u32* vp_words, u32 vp_word_count,
                          const rsx_nir_draw* d, const u32* batches)
{
    lane* const l = user;
    (void)st;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_DRAW);
    e->a = d->primitive;
    e->b = d->indexed;
    e->c = d->batch_count;
    e->d = d->total_count;
    e->payload_hash = fnv_bytes(0, batches,
                                (size_t)d->batch_count * 8u);
    e->payload_hash = fnv_bytes(e->payload_hash, vp_words,
                                (size_t)vp_word_count * 4u);
    return 0;
}

static int lane_exec_transfer(void* user, const rsx_nir_pipeline* st,
                              const rsx_nir_transfer* t, const u32* words)
{
    lane* const l = user;
    (void)st;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_TRANSFER);
    rsx_nir_transfer x = *t;
    x.words_ofs = 0;
    e->a = t->kind;
    e->payload_hash = fnv_bytes(0, &x, sizeof(x));
    if (t->word_count)
        e->payload_hash = fnv_bytes(e->payload_hash, words,
                                    (size_t)t->word_count * 4u);
    return 0;
}

static int lane_exec_present(void* user, u32 buffer)
{
    lane* const l = user;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_PRESENT);
    e->a = buffer;
    return 0;
}

static void lane_sem_write(void* user, u32 dma, u32 offset, u32 value,
                           u32 texture_read)
{
    lane* const l = user;
    action_log_entry* const e =
        lane_log_push(l, RSX_NIR_OP_SEMAPHORE_RELEASE);
    e->a = dma;
    e->b = offset;
    e->c = value;
    e->d = texture_read;
    if (dma == 0x66616661u && offset == 0x40u)
        l->semaphore_value = value;
}

static int lane_sem_read(void* user, u32 dma, u32 offset, u32* value)
{
    lane* const l = user;
    if (!value || dma != 0x66616661u || offset != 0x40u)
        return -1;
    *value = l->semaphore_value;
    return 0;
}

static int lane_report(void* user, u32 kind, u32 arg, u32 dma)
{
    lane* const l = user;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_REPORT);
    e->a = kind;
    e->b = arg;
    e->c = dma;
    return 0;
}

static void lane_set_reference(void* user, u32 value)
{
    lane* const l = user;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_SET_REFERENCE);
    e->a = value;
    l->references = value;
}

static void lane_user_command(void* user, u32 cause)
{
    lane* const l = user;
    action_log_entry* const e = lane_log_push(l, RSX_NIR_OP_USER_COMMAND);
    e->a = cause;
}

static int lane_read32(void* user, u32 io, u32* value)
{
    lane* const l = user;
    if (!value || (io & 3u) || io >= sizeof(l->words))
        return -1;
    *value = l->words[io >> 2];
    return 0;
}

static void lane_init(lane* l)
{
    memset(l, 0, sizeof(*l));
    rsx_nr_ring_init_fixed(&l->ring, l->slots, TEST_RING_OPS,
                           l->side, TEST_RING_SIDE);
    rsx_nr_tokens_init(&l->tokens);
    const rsx_nir_sink sink = rsx_nr_ring_sink(&l->ring);
    rsx_nir_adapter_init_sink(&l->adapter, &sink);
    /* the synthetic streams below are post-boot shaped, not the title's
     * exact context image */
    l->adapter.context_image_open = 0;
    rsx_nr_exec_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = l;
    ops.clear = lane_exec_clear;
    ops.draw = lane_exec_draw;
    ops.transfer = lane_exec_transfer;
    ops.present = lane_exec_present;
    ops.sem_write = lane_sem_write;
    ops.sem_read = lane_sem_read;
    ops.report = lane_report;
    ops.set_reference = lane_set_reference;
    ops.user_command = lane_user_command;
    rsx_nr_backend_init(&l->backend, &l->ring, &l->tokens, &ops);
    rsx_nr_frame_owner_init(&l->owner, &l->adapter, &l->backend, &l->ring,
                            lane_read32, l, NULL, NULL, NULL, NULL,
                            NULL, NULL, NULL, NULL, NULL, NULL);
}

/* island-compiler lane storage (static: the compiler struct is large) */
static rsx_nr_island_compiler g_ic;
static unsigned char g_arena[TEST_ARENA_BYTES];
static u32 g_index_lo[TEST_INDEX_CAP], g_index_hi[TEST_INDEX_CAP];
static u32 g_index_ofs[TEST_INDEX_CAP];
static rsx_nir_op g_scratch_ops[TEST_SCRATCH_OPS];
static u32 g_scratch_side[TEST_SCRATCH_SIDE];
static rsx_nir_adapter g_semantic_oracle_adapter;
static rsx_nir_op g_semantic_oracle_ops[TEST_SCRATCH_OPS];
static u32 g_semantic_oracle_side[TEST_SCRATCH_SIDE];

static int compiler_bind(lane* l)
{
    return rsx_nr_island_compiler_init(
        &g_ic, &l->owner, g_arena, TEST_ARENA_BYTES,
        g_index_lo, g_index_hi, g_index_ofs, TEST_INDEX_CAP,
        g_scratch_ops, TEST_SCRATCH_OPS,
        g_scratch_side, TEST_SCRATCH_SIDE);
}

/* ---- stream builder ---------------------------------------------------- */

typedef struct builder {
    lane* l;
    u32 io;
} builder;

static void b_init(builder* b, lane* l, u32 start)
{
    b->l = l;
    b->io = start;
}

static void b_word(builder* b, u32 w)
{
    b->l->words[b->io >> 2] = w;
    b->io += 4u;
}

static void b_packet(builder* b, u32 method, u32 count, const u32* args)
{
    b_word(b, (count << 18) | (method & 0x3FFFCu));
    for (u32 i = 0; i < count; ++i)
        b_word(b, args[i]);
}

static void b_packet_ni(builder* b, u32 method, u32 count, const u32* args)
{
    b_word(b, 0x40000000u | (count << 18) | (method & 0x3FFFCu));
    for (u32 i = 0; i < count; ++i)
        b_word(b, args[i]);
}

static void b_m1(builder* b, u32 method, u32 arg)
{
    b_packet(b, method, 1, &arg);
}

static void b_finish(builder* b)
{
    b->l->put = b->io;
}

/* Drive one lane until GET reaches PUT (or a wait/fatal). Returns the
 * final step result observed. */
static rsx_nr_frame_step_result lane_run(lane* l, int use_compiler,
                                         u32* get_io, u32 max_steps)
{
    u32 get = *get_io;
    u32 ret = ~0u;
    rsx_nr_frame_step_result r = RSX_NR_FRAME_WAIT_EMPTY;
    for (u32 i = 0; i < max_steps; ++i) {
        u32 next_get = get, next_ret = ret;
        r = use_compiler
            ? rsx_nr_island_compiler_step(&g_ic, get, l->put, ret,
                                          &next_get, &next_ret)
            : rsx_nr_frame_owner_step(&l->owner, get, l->put, ret,
                                      &next_get, &next_ret);
        get = next_get;
        ret = next_ret;
        if (r == RSX_NR_FRAME_FATAL)
            break;
        if (get == l->put &&
            (r == RSX_NR_FRAME_WAIT_EMPTY || r == RSX_NR_FRAME_ADVANCED)) {
            if (r == RSX_NR_FRAME_WAIT_EMPTY)
                break;
            continue;
        }
        if (r == RSX_NR_FRAME_WAIT_SEMAPHORE ||
            r == RSX_NR_FRAME_WAIT_STOPPER)
            break;
    }
    *get_io = get;
    return r;
}

static int logs_equal(const lane* a, const lane* b, const char* what)
{
    if (a->log_count != b->log_count) {
        fprintf(stderr, "FAIL: %s: action counts differ %u vs %u\n", what,
                a->log_count, b->log_count);
        return 0;
    }
    for (u32 i = 0; i < a->log_count; ++i) {
        if (memcmp(&a->log[i], &b->log[i], sizeof(a->log[i])) != 0) {
            fprintf(stderr,
                    "FAIL: %s: action %u differs "
                    "(kind %u/%u a=%08X/%08X b=%08X/%08X c=%08X/%08X "
                    "d=%08X/%08X state=%016llX/%016llX "
                    "payload=%016llX/%016llX)\n",
                    what, i, a->log[i].kind, b->log[i].kind,
                    a->log[i].a, b->log[i].a, a->log[i].b, b->log[i].b,
                    a->log[i].c, b->log[i].c, a->log[i].d, b->log[i].d,
                    (unsigned long long)a->log[i].state_hash,
                    (unsigned long long)b->log[i].state_hash,
                    (unsigned long long)a->log[i].payload_hash,
                    (unsigned long long)b->log[i].payload_hash);
            return 0;
        }
    }
    return 1;
}

/* ---- shared synthetic content ------------------------------------------ */

/* A "frame": state, a constant upload, a textured indexed draw, a clear,
 * a semaphore release, a reference, a report, a user command, a present.
 * Dynamic knobs let tests vary values without changing structure. */
typedef struct frame_knobs {
    u32 color_offset;
    u32 blend_enable;           /* structural when changed                 */
    u32 const_value;
    u32 draw_first;
    u32 draw_count2;            /* second batch count byte                 */
    u32 clear_color;
    u32 sem_value;
    u32 ref_value;
    u32 report_arg;
    u32 cause;
    u32 tex_offset;
    u32 index_offset;
} frame_knobs;

static void build_frame(builder* b, const frame_knobs* k)
{
    /* surface + viewport-ish state */
    b_m1(b, 0x0208u, 0x00000505u);           /* RT format A8R8G8B8+Z24    */
    b_m1(b, 0x0200u, 0x02800000u);           /* RT horiz 640              */
    b_m1(b, 0x0204u, 0x01E00000u);           /* RT vert 480               */
    b_m1(b, 0x0210u, k->color_offset);       /* color0 offset (dynamic)   */
    b_m1(b, 0x020Cu, 0x00000A00u);           /* color0 pitch              */
    b_m1(b, 0x0310u, k->blend_enable);       /* blend enable              */
    b_m1(b, 0x031Cu, 0x40404040u);           /* blend color (dynamic)     */
    /* one vec4 transform constant */
    {
        u32 args[5] = { 7u, k->const_value, k->const_value + 1u,
                        k->const_value + 2u, k->const_value + 3u };
        b_word(b, (5u << 18) | 0x1EFCu);     /* CONST_ID + 4 words        */
        for (u32 i = 0; i < 5u; ++i)
            b_word(b, args[i]);
    }
    /* texture unit 2 */
    b_m1(b, 0x1A40u, k->tex_offset);         /* offset (dynamic)          */
    b_m1(b, 0x1A44u, 0x0002A585u);           /* format                    */
    /* indexed draw, two batches */
    b_m1(b, 0x181Cu, k->index_offset);       /* index address (dynamic)   */
    b_m1(b, 0x1820u, 0x00000010u);           /* index format u16 + local  */
    b_m1(b, 0x1808u, 5u);                    /* BEGIN triangles           */
    {
        u32 batches[2] = {
            (k->draw_first & 0xFFFFFFu) | (11u << 24),
            ((k->draw_first + 12u) & 0xFFFFFFu) |
                ((k->draw_count2 & 0xFFu) << 24)
        };
        b_packet_ni(b, 0x1824u, 2u, batches); /* two index batches        */
    }
    b_m1(b, 0x1808u, 0u);                    /* END -> draw               */
    /* clear */
    b_m1(b, 0x1D90u, k->clear_color);
    b_m1(b, 0x1D8Cu, 0xFFFFFF00u);
    b_m1(b, 0x1D94u, 0xF3u);                 /* mask                      */
    /* NV406E semaphore offset + release (dynamic offset/value) */
    b_m1(b, 0x0064u, 0x40u);
    b_m1(b, 0x006Cu, k->sem_value);
    /* reference */
    b_m1(b, 0x0050u, k->ref_value);
    /* report */
    b_m1(b, 0x1800u, k->report_arg);
    /* user command */
    b_m1(b, 0xEB00u, k->cause);
    /* present */
    b_m1(b, 0xE944u, 1u);
}

static void default_knobs(frame_knobs* k)
{
    memset(k, 0, sizeof(*k));
    k->color_offset = 0x00100000u;
    k->blend_enable = 1u;
    k->const_value = 0x3F800000u;
    k->draw_first = 100u;
    k->draw_count2 = 7u;
    k->clear_color = 0x11223344u;
    k->sem_value = 0x55u;
    k->ref_value = 0x1234u;
    k->report_arg = 0x00000120u;
    k->cause = 0xCAFE0001u;
    k->tex_offset = 0x00200000u;
    k->index_offset = 0x00030000u;
}

/* ---- tests ------------------------------------------------------------- */

static lane g_oracle, g_subject;

static int test_equivalence_and_replay(void)
{
    lane* const o = &g_oracle;
    lane* const s = &g_subject;
    frame_knobs k;
    default_knobs(&k);

    lane_init(o);
    builder bo;
    b_init(&bo, o, 0x1000u);
    build_frame(&bo, &k);
    b_finish(&bo);
    u32 get = 0x1000u;
    rsx_nr_frame_step_result r = lane_run(o, 0, &get, 100000u);
    CHECK(r != RSX_NR_FRAME_FATAL, "oracle lane faulted kind=%u m=%05X",
          o->owner.failure.kind, o->owner.failure.method);
    CHECK(get == o->put, "oracle did not consume the stream");
    CHECK(o->log_count > 0 && !o->log_overflow, "oracle log empty");

    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    rsx_nr_island_compiler_set_oracle(
        &g_ic, &g_semantic_oracle_adapter,
        g_semantic_oracle_ops, TEST_SCRATCH_OPS,
        g_semantic_oracle_side, TEST_SCRATCH_SIDE);
    builder bs;
    b_init(&bs, s, 0x1000u);
    build_frame(&bs, &k);
    b_finish(&bs);
    get = 0x1000u;
    r = lane_run(s, 1, &get, 100000u);
    CHECK(r != RSX_NR_FRAME_FATAL, "compiler lane faulted kind=%u m=%05X",
          s->owner.failure.kind, s->owner.failure.method);
    CHECK(get == s->put, "compiler did not consume the stream");
    CHECK(logs_equal(o, s, "first pass"), "lane divergence");
    CHECK(g_ic.stats.islands_compiled > 0, "nothing compiled");
    CHECK(g_ic.stats.islands_hit == 0, "unexpected hit on cold pass");
    const unsigned long long compiled_cold = g_ic.stats.islands_compiled;

    /* replay the identical content at a fresh FIFO position: everything
     * must hit, no recompiles, identical actions */
    const u32 replay_base = 0x8000u;
    builder br;
    b_init(&br, s, replay_base);
    build_frame(&br, &k);
    b_finish(&br);
    const u32 log_before = s->log_count;
    get = replay_base;
    r = lane_run(s, 1, &get, 100000u);
    CHECK(r != RSX_NR_FRAME_FATAL, "replay faulted");
    CHECK(get == s->put, "replay did not consume");
    CHECK(g_ic.stats.islands_compiled == compiled_cold,
          "replay recompiled templates");
    CHECK(g_ic.stats.islands_hit >= compiled_cold,
          "replay did not hit templates (hits=%llu)",
          (unsigned long long)g_ic.stats.islands_hit);
    CHECK(g_ic.stats.adaptations_avoided > 0, "no adaptation avoided");
    CHECK(g_ic.oracle_stats.action_islands_checked > 0,
          "semantic oracle did not check action islands");
    CHECK(g_ic.oracle_stats.mismatches == 0,
          "semantic oracle found %llu mismatch(es), first reason=%u",
          g_ic.oracle_stats.mismatches,
          g_ic.oracle_stats.first_reason);
    CHECK(s->log_count == log_before * 2u, "replay action count");
    for (u32 i = 0; i < log_before; ++i)
        CHECK(memcmp(&s->log[i], &s->log[log_before + i],
                     sizeof(s->log[i])) == 0,
              "replay action %u differs from first pass", i);
    printf("equivalence+replay: islands=%llu hits=%llu methods-avoided=%llu "
           "groups-derived=%llu\n",
           (unsigned long long)g_ic.stats.islands_compiled,
           (unsigned long long)g_ic.stats.islands_hit,
           (unsigned long long)g_ic.stats.adaptations_avoided,
           (unsigned long long)g_ic.stats.groups_derived);
    return 0;
}

static int test_owned_island_resets_flow_streak(void)
{
    lane* const s = &g_subject;
    frame_knobs k;
    default_knobs(&k);
    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");

    /* Reproduce the live failure boundary directly: delegated flow packets
     * had raised the strict owner's bounded streak to its limit even though
     * compiler-owned ordinary islands occurred between them.  A proven,
     * fully owned non-flow island must reset that streak exactly as the
     * ordinary owner packet path does. */
    s->owner.control_streak = 4096u;
    builder b;
    b_init(&b, s, 0x1000u);
    build_frame(&b, &k);
    b_finish(&b);
    u32 get = 0x1000u;
    u32 next = get, nret = ~0u;
    const rsx_nr_frame_step_result r = rsx_nr_island_compiler_step(
        &g_ic, get, s->put, ~0u, &next, &nret);
    CHECK(r == RSX_NR_FRAME_ADVANCED,
          "owned island did not advance at flow limit (%u)", r);
    CHECK(next > get, "owned island left GET unchanged");
    CHECK(s->owner.control_streak == 0u,
          "owned island retained flow streak %u", s->owner.control_streak);
    CHECK(!s->owner.fatal, "owned island caused false BAD_FLOW fatal");
    return 0;
}

static int test_dynamic_change_no_recompile(void)
{
    lane* const o = &g_oracle;
    lane* const s = &g_subject;
    frame_knobs k;
    default_knobs(&k);

    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder bs;
    b_init(&bs, s, 0x1000u);
    build_frame(&bs, &k);
    b_finish(&bs);
    u32 get = 0x1000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL,
          "cold pass faulted");
    const unsigned long long compiled_cold = g_ic.stats.islands_compiled;
    const u32 log_cold = s->log_count;

    /* every dynamic knob changes; structure does not */
    k.color_offset = 0x00180000u;
    k.const_value = 0x40490FDBu;
    k.draw_first = 4242u;
    k.draw_count2 = 200u;
    k.clear_color = 0xDEADBEEFu;
    k.sem_value = 0xABCDu;
    k.ref_value = 0x9999u;
    k.report_arg = 0x00000480u;
    k.cause = 0xCAFE0002u;
    k.tex_offset = 0x00280000u;
    k.index_offset = 0x00038000u;

    builder br;
    b_init(&br, s, 0x8000u);
    build_frame(&br, &k);
    b_finish(&br);
    get = 0x8000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL,
          "dynamic pass faulted");
    CHECK(get == s->put, "dynamic pass did not consume");
    CHECK(g_ic.stats.islands_compiled == compiled_cold,
          "dynamic-only change recompiled a template");
    CHECK(g_ic.stats.validation_mismatches == 0,
          "dynamic-only change hit validation mismatch");

    /* oracle for the changed content proves values flowed through */
    lane_init(o);
    builder bo;
    b_init(&bo, o, 0x8000u);
    build_frame(&bo, &k);
    b_finish(&bo);
    get = 0x8000u;
    CHECK(lane_run(o, 0, &get, 100000u) != RSX_NR_FRAME_FATAL,
          "oracle faulted");
    CHECK(o->log_count == s->log_count - log_cold, "action count");
    for (u32 i = 0; i < o->log_count; ++i)
        CHECK(memcmp(&o->log[i], &s->log[log_cold + i],
                     sizeof(o->log[i])) == 0,
              "patched action %u differs from oracle", i);
    printf("dynamic-change: hits=%llu patched-slots=%llu\n",
           (unsigned long long)g_ic.stats.islands_hit,
           (unsigned long long)g_ic.stats.constants_slots_patched);
    return 0;
}

static int test_structural_change_recompiles(void)
{
    lane* const s = &g_subject;
    frame_knobs k;
    default_knobs(&k);

    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder b1;
    b_init(&b1, s, 0x1000u);
    build_frame(&b1, &k);
    b_finish(&b1);
    u32 get = 0x1000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL, "cold");
    const unsigned long long compiled_cold = g_ic.stats.islands_compiled;

    k.blend_enable = 0u;                     /* structural change         */
    builder b2;
    b_init(&b2, s, 0x8000u);
    build_frame(&b2, &k);
    b_finish(&b2);
    get = 0x8000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL, "warm");
    CHECK(g_ic.stats.islands_compiled > compiled_cold,
          "structural change did not recompile");
    return 0;
}

static int test_unsupported_island_delegates(void)
{
    lane* const o = &g_oracle;
    lane* const s = &g_subject;

    /* state then an unsupported (stored-only) method inside a called list
     * (ret != ~0u), which the strict owner refuses immediately: both lanes
     * must fault identically, and the compiler must own none of it. */
    lane_init(o);
    builder bo;
    b_init(&bo, o, 0x1000u);
    b_m1(&bo, 0x0310u, 1u);
    b_m1(&bo, 0x0234u, 0x1234u);             /* not a modeled method      */
    b_finish(&bo);
    u32 get = 0x1000u;
    u32 next = get, nret = 0x9000u;
    rsx_nr_frame_step_result r1 = RSX_NR_FRAME_WAIT_EMPTY;
    for (u32 i = 0; i < 64 && r1 != RSX_NR_FRAME_FATAL; ++i)
        r1 = rsx_nr_frame_owner_step(&o->owner, next, o->put, 0x9000u,
                                     &next, &nret);
    CHECK(r1 == RSX_NR_FRAME_FATAL, "oracle accepted unsupported method");
    CHECK(o->owner.failure.kind == RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD,
          "oracle failure kind %u", o->owner.failure.kind);

    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder bs;
    b_init(&bs, s, 0x1000u);
    b_m1(&bs, 0x0310u, 1u);
    b_m1(&bs, 0x0234u, 0x1234u);
    b_finish(&bs);
    next = 0x1000u;
    nret = 0x9000u;
    rsx_nr_frame_step_result r2 = RSX_NR_FRAME_WAIT_EMPTY;
    for (u32 i = 0; i < 64 && r2 != RSX_NR_FRAME_FATAL; ++i)
        r2 = rsx_nr_island_compiler_step(&g_ic, next, s->put, 0x9000u,
                                         &next, &nret);
    CHECK(r2 == RSX_NR_FRAME_FATAL, "compiler accepted unsupported method");
    CHECK(s->owner.failure.kind == RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD,
          "compiler failure kind %u", s->owner.failure.kind);
    CHECK(s->owner.failure.method == o->owner.failure.method,
          "failure method mismatch");
    CHECK(g_ic.stats.methods_owned == 0,
          "compiler owned part of an unsupported island");
    CHECK(g_ic.stats.islands_delegated[RSX_NR_ISLAND_DELEGATE_UNSUPPORTED]
              > 0, "unsupported island not counted");
    return 0;
}

static int test_blocked_acquire_resume(void)
{
    lane* const s = &g_subject;
    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder b;
    b_init(&b, s, 0x1000u);
    b_m1(&b, 0x0064u, 0x40u);                /* semaphore offset          */
    b_m1(&b, 0x0068u, 0x77u);                /* acquire (unsatisfied)     */
    b_m1(&b, 0x0050u, 0x1u);                 /* reference after           */
    b_finish(&b);
    u32 get = 0x1000u;
    u32 next = get, nret = ~0u;
    rsx_nr_frame_step_result r = RSX_NR_FRAME_ADVANCED;
    /* the offset packet closes as a state-only island first; the acquire
     * island itself must then block with GET unmoved */
    for (u32 i = 0; i < 8u && r == RSX_NR_FRAME_ADVANCED; ++i) {
        get = next;
        r = rsx_nr_island_compiler_step(&g_ic, get, s->put, ~0u,
                                        &next, &nret);
    }
    CHECK(r == RSX_NR_FRAME_WAIT_SEMAPHORE, "expected blocked acquire, %u",
          r);
    CHECK(next == get, "GET advanced across a blocked acquire");
    CHECK(s->references == 0u, "reference ran before the acquire");
    /* a retry while still unsatisfied stays blocked at the same GET */
    r = rsx_nr_island_compiler_step(&g_ic, get, s->put, ~0u, &next, &nret);
    CHECK(r == RSX_NR_FRAME_WAIT_SEMAPHORE && next == get,
          "blocked retry misbehaved");
    /* satisfy and resume */
    s->semaphore_value = 0x77u;
    r = rsx_nr_island_compiler_step(&g_ic, get, s->put, ~0u, &next, &nret);
    CHECK(r == RSX_NR_FRAME_ADVANCED, "resume failed %u", r);
    get = next;
    r = lane_run(s, 1, &get, 1000u);
    CHECK(r != RSX_NR_FRAME_FATAL && get == s->put, "tail failed");
    CHECK(s->references == 1u, "post-acquire reference lost");
    return 0;
}

static int test_invalidate_and_generation(void)
{
    lane* const s = &g_subject;
    frame_knobs k;
    default_knobs(&k);
    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder b1;
    b_init(&b1, s, 0x1000u);
    build_frame(&b1, &k);
    b_finish(&b1);
    u32 get = 0x1000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL, "cold");
    const unsigned long long compiled_cold = g_ic.stats.islands_compiled;

    /* identical replay with unchanged content generation takes the
     * generation fast path */
    builder b2;
    b_init(&b2, s, 0x1000u);
    build_frame(&b2, &k);
    b_finish(&b2);
    get = 0x1000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL, "warm");
    CHECK(g_ic.stats.generation_fast_hits > 0, "no generation fast hits");

    /* a content write forces revalidation (still hits, no recompiles) */
    rsx_nr_island_compiler_note_content_write(&g_ic);
    const unsigned long long fast_before = g_ic.stats.generation_fast_hits;
    get = 0x1000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL, "warm2");
    CHECK(g_ic.stats.generation_fast_hits == fast_before,
          "content write did not force revalidation");
    CHECK(g_ic.stats.islands_compiled == compiled_cold, "revalidation "
          "recompiled");

    /* invalidate-all drops every template */
    rsx_nr_island_compiler_invalidate_all(&g_ic);
    CHECK(g_ic.stats.templates_live == 0, "templates survived invalidate");
    get = 0x1000u;
    CHECK(lane_run(s, 1, &get, 100000u) != RSX_NR_FRAME_FATAL, "post-inv");
    CHECK(g_ic.stats.islands_compiled > compiled_cold,
          "invalidate did not recompile");
    return 0;
}

static int test_entry_const_pointer_identity(void)
{
    lane* const o = &g_oracle;
    lane* const s = &g_subject;

    /* island A sets CONST_ID; island B (separated by a barrier action)
     * uploads without setting it. Changing A's pointer must give B a new
     * template (the latched entry pointer), landing values in new slots. */
    for (u32 variant = 0; variant < 2u; ++variant) {
        const u32 base_slot = variant ? 9u : 5u;
        lane* const lanes[2] = { o, s };
        for (u32 which = 0; which < 2u; ++which) {
            lane* const l = lanes[which];
            if (which == 0) {
                lane_init(l);
            } else {
                lane_init(l);
                CHECK(compiler_bind(l) == 0, "compiler init failed");
            }
            builder b;
            b_init(&b, l, 0x1000u);
            b_m1(&b, 0x1EFCu, base_slot);    /* island A                  */
            b_m1(&b, 0x0110u, 0u);           /* barrier ends island A     */
            {
                u32 args[4] = { 0x11u + variant, 0x22u, 0x33u, 0x44u };
                b_packet(&b, 0x1F00u, 4u, args);   /* island B upload     */
            }
            b_m1(&b, 0x0110u, 0u);           /* barrier ends island B     */
            b_finish(&b);
            u32 get = 0x1000u;
            CHECK(lane_run(l, which, &get, 10000u) != RSX_NR_FRAME_FATAL,
                  "const-pointer lane faulted");
            CHECK(get == l->put, "const-pointer lane did not consume");
        }
        CHECK(logs_equal(o, s, "const-pointer"), "const-pointer variant %u",
              variant);
        /* the folded state hash inside the logs proves slot placement */
    }
    return 0;
}

static int test_inline_transfer_island(void)
{
    lane* const o = &g_oracle;
    lane* const s = &g_subject;
    for (int which = 0; which < 2; ++which) {
        lane* const l = which ? s : o;
        lane_init(l);
        if (which)
            CHECK(compiler_bind(l) == 0, "compiler init failed");
        builder b;
        b_init(&b, l, 0x1000u);
        b_m1(&b, 0x6300u, 0xBu);             /* s2d format                */
        b_m1(&b, 0x6304u, (64u << 16) | 64u);/* s2d pitch                 */
        b_m1(&b, 0x630Cu, 0x00040000u);      /* s2d dst offset            */
        b_m1(&b, 0xA304u, 0x00000000u);      /* point                     */
        b_m1(&b, 0xA308u, 0x00010008u);      /* size out 8x1              */
        {
            u32 words[6] = { 1, 2, 3, 4, 5, 6 };
            b_packet(&b, 0xA400u, 6u, words);/* payload run               */
        }
        b_m1(&b, 0x0050u, 0x42u);            /* reference closes the run  */
        b_finish(&b);
        u32 get = 0x1000u;
        CHECK(lane_run(l, which, &get, 10000u) != RSX_NR_FRAME_FATAL,
              "inline lane faulted (owner failure kind=%u method=%05X)",
              l->owner.failure.kind, l->owner.failure.method);
        CHECK(get == l->put, "inline lane did not consume");
    }
    CHECK(logs_equal(o, s, "inline-transfer"), "inline transfer");
    CHECK(g_ic.stats.islands_compiled >= 2, "inline island not compiled");
    return 0;
}

static int test_invalidate_during_blocked_acquire(void)
{
    lane* const s = &g_subject;
    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder b;
    b_init(&b, s, 0x1000u);
    b_m1(&b, 0x0064u, 0x40u);
    b_m1(&b, 0x0050u, 0x11u);                /* pre-acquire reference     */
    b_m1(&b, 0x0068u, 0x77u);                /* acquire (unsatisfied)     */
    b_m1(&b, 0x0050u, 0x22u);                /* post-acquire reference    */
    b_finish(&b);
    u32 get = 0x1000u, next = get, nret = ~0u;
    rsx_nr_frame_step_result r = RSX_NR_FRAME_ADVANCED;
    for (u32 i = 0; i < 8u && r == RSX_NR_FRAME_ADVANCED; ++i) {
        get = next;
        r = rsx_nr_island_compiler_step(&g_ic, get, s->put, ~0u,
                                        &next, &nret);
    }
    CHECK(r == RSX_NR_FRAME_WAIT_SEMAPHORE, "expected block, %u", r);
    const u32 refs_at_block = s->log_count;
    /* templates drop underneath the retained island; nothing may re-run */
    rsx_nr_island_compiler_invalidate_all(&g_ic);
    CHECK(g_ic.stats.templates_live == 0, "invalidate kept templates");
    s->semaphore_value = 0x77u;
    r = rsx_nr_island_compiler_step(&g_ic, get, s->put, ~0u, &next, &nret);
    CHECK(r == RSX_NR_FRAME_ADVANCED, "resume after invalidate failed %u",
          r);
    get = next;
    r = lane_run(s, 1, &get, 1000u);
    CHECK(r != RSX_NR_FRAME_FATAL && get == s->put, "tail failed");
    CHECK(s->references == 0x22u, "post-acquire work lost");
    /* pre-acquire reference executed exactly once (no re-execution) */
    u32 ref_count = 0;
    for (u32 i = 0; i < s->log_count; ++i)
        if (s->log[i].kind == RSX_NIR_OP_SET_REFERENCE &&
            s->log[i].a == 0x11u)
            ref_count++;
    CHECK(ref_count == 1u, "pre-acquire reference ran %u times", ref_count);
    (void)refs_at_block;
    return 0;
}

static int test_partial_publication_waits(void)
{
    lane* const s = &g_subject;
    frame_knobs k;
    default_knobs(&k);
    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");
    builder b;
    b_init(&b, s, 0x1000u);
    build_frame(&b, &k);
    const u32 full_put = b.io;
    /* publish only up to the middle of the draw packet chain */
    s->put = 0x1000u + 12u * 4u;
    u32 get = 0x1000u;
    u32 next = get, nret = ~0u;
    rsx_nr_frame_step_result r =
        rsx_nr_island_compiler_step(&g_ic, get, s->put, ~0u, &next, &nret);
    CHECK(r == RSX_NR_FRAME_WAIT_PARTIAL || r == RSX_NR_FRAME_WAIT_EMPTY ||
          r == RSX_NR_FRAME_ADVANCED,
          "partial publication mis-stepped %u", r);
    CHECK(g_ic.stats.actions_executed == 0,
          "action executed before its island was fully published");
    /* full publication completes normally */
    s->put = full_put;
    get = next;
    r = lane_run(s, 1, &get, 100000u);
    CHECK(r != RSX_NR_FRAME_FATAL && get == s->put, "completion failed");
    CHECK(g_ic.stats.actions_executed > 0, "no actions after publication");
    return 0;
}

/* The classification split must mirror the canonical-PSO identity rule:
 * values outside PSO identity plus addresses and payloads are dynamic,
 * structure is not (docs/HANA_ISLAND_COMPILER.md). */
static int test_dynamic_classification_table(void)
{
    static const struct { u32 method; int dynamic; } cases[] = {
        { 0x0210u, 1 },   /* color0 offset: address                        */
        { 0x0208u, 0 },   /* RT format: structure                          */
        { 0x1F00u, 1 },   /* transform-constant payload                    */
        { 0x1EFCu, 0 },   /* constant load pointer: slot structure         */
        { 0x1808u, 0 },   /* BEGIN_END: primitive identity                 */
        { 0x1814u, 1 },   /* draw batch words                              */
        { 0x0B80u, 0 },   /* VP instruction words: program identity        */
        { 0x1D90u, 1 },   /* clear color value                             */
        { 0x1D94u, 0 },   /* clear mask                                    */
        { 0x0310u, 0 },   /* blend enable: PSO identity                    */
        { 0x031Cu, 1 },   /* blend color: dynamic state                    */
        { 0x1A40u, 1 },   /* texture unit 2 offset                         */
        { 0x1A44u, 0 },   /* texture unit 2 format                         */
        { 0xA308u, 0 },   /* NV308A SIZE_OUT: inline run shaping           */
        { 0x0050u, 1 },   /* reference value                               */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        CHECK(rsx_nr_island_method_arg_is_dynamic(cases[i].method) ==
                  cases[i].dynamic,
              "classification of %05X", cases[i].method);
    return 0;
}

static void build_simple_draw(builder* b)
{
    b_m1(b, 0x1808u, 5u);
    b_m1(b, 0x1814u, 0x00000003u);
    b_m1(b, 0x1808u, 0u);
}

/* A capacity/grammar delegation may consume ordinary state without an
 * action. The strict adapter intentionally delays that state until its next
 * action, while the next action may be compiler-owned. Reproduce the live
 * RT-height divergence and prove the compiler performs one complete catchup
 * instead of drawing with the backend's stale surface. */
static int test_delegated_state_catches_up_before_owned_action(void)
{
    lane* const s = &g_subject;
    lane_init(s);
    CHECK(compiler_bind(s) == 0, "compiler init failed");

    builder first;
    b_init(&first, s, 0x1000u);
    build_simple_draw(&first);
    b_finish(&first);
    u32 get = 0x1000u;
    rsx_nr_frame_step_result r = lane_run(s, 1, &get, 1000u);
    CHECK(r != RSX_NR_FRAME_FATAL && get == s->put,
          "initial draw did not compile");
    CHECK(g_ic.stats.islands_compiled != 0u, "draw template missing");

    /* Force only this new state island through the strict owner. The NOP
     * closes the state-only extent without adding another adapter method. */
    g_ic.index_live = (g_ic.index_cap / 4u) * 3u;
    builder state;
    b_init(&state, s, 0x4000u);
    b_m1(&state, 0x0204u, 0x00800000u); /* RT clip height = 128 */
    b_word(&state, 0u);
    b_finish(&state);
    get = 0x4000u;
    r = lane_run(s, 1, &get, 1000u);
    CHECK(r != RSX_NR_FRAME_FATAL && get == s->put,
          "delegated state island did not complete");
    CHECK(g_ic.force_full_state != 0u,
          "delegated state did not arm backend catchup");

    builder replay;
    b_init(&replay, s, 0x8000u);
    build_simple_draw(&replay);
    b_finish(&replay);
    get = 0x8000u;
    r = lane_run(s, 1, &get, 1000u);
    CHECK(r != RSX_NR_FRAME_FATAL && get == s->put,
          "owned replay draw did not complete");
    CHECK(s->backend.st.surface.clip_h == 128u,
          "owned draw retained stale RT height %u",
          s->backend.st.surface.clip_h);
    CHECK(g_ic.force_full_state == 0u,
          "successful catchup remained armed");
    return 0;
}

int main(void)
{
    struct {
        const char* name;
        int (*fn)(void);
    } tests[] = {
        { "classification-table", test_dynamic_classification_table },
        { "equivalence+replay", test_equivalence_and_replay },
        { "owned-island-flow-reset", test_owned_island_resets_flow_streak },
        { "dynamic-change", test_dynamic_change_no_recompile },
        { "structural-change", test_structural_change_recompiles },
        { "unsupported-delegates", test_unsupported_island_delegates },
        { "blocked-acquire", test_blocked_acquire_resume },
        { "invalidate+generation", test_invalidate_and_generation },
        { "invalidate-blocked-acquire", test_invalidate_during_blocked_acquire },
        { "entry-const-pointer", test_entry_const_pointer_identity },
        { "inline-transfer", test_inline_transfer_island },
        { "partial-publication", test_partial_publication_waits },
        { "delegated-state-catchup",
          test_delegated_state_catches_up_before_owned_action },
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].fn()) {
            fprintf(stderr, "island suite FAILED at %s\n", tests[i].name);
            return 1;
        }
        printf("ok: %s\n", tests[i].name);
    }
    printf("island compiler suite passed\n");
    return 0;
}
