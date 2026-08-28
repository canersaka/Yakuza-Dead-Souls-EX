/* Strict native RSX frame owner. See rsx_nr_frame_owner.h. */

#include "rsx_nr_frame_owner.h"

#include <string.h>

static int frame_graph_exec_mode(const rsx_nr_frame_owner* o)
{
    return o && (o->graph_mode == RSX_NR_FRAME_GRAPH_EXECUTE ||
                 o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT);
}

#define NR_FRAME_RING_SIZE 0x800000u
#define NR_FRAME_RING_MASK (NR_FRAME_RING_SIZE - 1u)
#define NR_FRAME_METHOD_OP_BOUND 512u
#define NR_FRAME_METHOD_SIDE_BOUND 16384u
#define NR_FRAME_CONTROL_BOUND 4096u
#define NR_FRAME_FLOW_WAIT_BOUND (1u << 24)
#define NR_FRAME_GENERATED_LINK_PROOF_DELAY (1u << 16)
#define NR_FRAME_PUBLICATION_CLOCK_POLL_INTERVAL (1u << 16)
#define NR_FRAME_PUBLICATION_PROOF_DELAY_MS 2u
#define NR_FRAME_PUBLICATION_FAILURE_DELAY_MS 30000u
#define NR_FRAME_PRIMARY_SEGMENT_BYTES 0x100000u
#define NR_FRAME_GENERATED_BLOCK_BYTES 0x20000u
static rsx_nr_frame_step_result frame_fail(
    rsx_nr_frame_owner* o, u32 kind, u32 get, u32 put, u32 ret, u32 command,
    u32 method, u32 argument, u32 index)
{
    if (!o->fatal) {
        o->failure.kind = kind;
        o->failure.get = get;
        o->failure.put = put;
        o->failure.call_return = ret;
        o->failure.command = command;
        o->failure.method = method;
        o->failure.argument = argument;
        o->failure.argument_index = index;
        o->failure.frame = o->stats.frames;
    }
    o->fatal = 1;
    return RSX_NR_FRAME_FATAL;
}

static int frame_read(rsx_nr_frame_owner* o, u32 io, u32* value)
{
    return o->read32 && o->read32(o->read_user, io, value) == 0;
}

static u32 frame_linear_next(u32 pc, u32 bytes, u32 ret)
{
    return ret == ~0u ? ((pc + bytes) & NR_FRAME_RING_MASK)
                      : pc + bytes;
}

static rsx_nr_frame_step_result frame_control_advance(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 ret, u32 command)
{
    o->stats.control_words++;
    if (++o->control_streak > NR_FRAME_CONTROL_BOUND)
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, ret, command,
            0u, 0u, o->control_streak);
    return RSX_NR_FRAME_ADVANCED;
}

void rsx_nr_frame_owner_init(rsx_nr_frame_owner* o,
                             rsx_nir_adapter* adapter,
                             rsx_nr_backend* backend,
                             rsx_nr_ring* ring,
                             rsx_nr_frame_read32_fn read32,
                             void* read_user,
                             rsx_nr_frame_release_stopper_fn release_stopper,
                             void* release_stopper_user,
                             rsx_nr_frame_island_edge_fn island_edge,
                             void* island_edge_user,
                             rsx_nr_frame_released_edge_fn released_edge,
                             void* released_edge_user,
                             rsx_nr_frame_resolve_jump_fn resolve_jump,
                             void* resolve_jump_user,
                             rsx_nr_frame_resolve_hole_fn resolve_hole,
                             void* resolve_hole_user)
{
    memset(o, 0, sizeof(*o));
    o->adapter = adapter;
    o->backend = backend;
    o->ring = ring;
    o->read32 = read32;
    o->read_user = read_user;
    o->release_stopper = release_stopper;
    o->release_stopper_user = release_stopper_user;
    o->island_edge = island_edge;
    o->island_edge_user = island_edge_user;
    o->released_edge = released_edge;
    o->released_edge_user = released_edge_user;
    o->resolve_jump = resolve_jump;
    o->resolve_jump_user = resolve_jump_user;
    o->resolve_hole = resolve_hole;
    o->resolve_hole_user = resolve_hole_user;
    o->breadcrumb_last_get = ~0u;
    o->breadcrumb_last_return = ~0u;
    o->breadcrumb_last_command = ~0u;
    o->flow_wait_source = ~0u;
    o->flow_wait_target = ~0u;
    o->flow_wait_put = ~0u;
    o->flow_wait_limit = NR_FRAME_FLOW_WAIT_BOUND;
    o->primary_segment_bytes = NR_FRAME_PRIMARY_SEGMENT_BYTES;
    o->generated_block_bytes = NR_FRAME_GENERATED_BLOCK_BYTES;
}

void rsx_nr_frame_owner_set_publication_clock(
    rsx_nr_frame_owner* o, rsx_nr_frame_now_ms_fn now_ms,
    void* user, u32 proof_delay_ms, u32 failure_delay_ms)
{
    if (!o)
        return;
    o->publication_now_ms = now_ms;
    o->publication_clock_user = user;
    o->publication_proof_delay_ms = proof_delay_ms
        ? proof_delay_ms : NR_FRAME_PUBLICATION_PROOF_DELAY_MS;
    o->publication_failure_delay_ms = failure_delay_ms
        ? failure_delay_ms : NR_FRAME_PUBLICATION_FAILURE_DELAY_MS;
    o->flow_wait_clock_next_poll = 0u;
}

static int frame_graph_sink_push(void* user, const rsx_nir_op* op)
{
    rsx_nr_frame_owner* const o = (rsx_nr_frame_owner*)user;
    if (!o || !o->graph_stream || rsx_nir_push(o->graph_stream, op) != 0)
        return -1;
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT) {
        if (!o->graph_record_initialized) {
            o->graph_record_backend = *o->backend;
            o->graph_record_initialized = 1u;
        }
        const u32 op_index = o->graph_stream->op_count - 1u;
        if (rsx_nr_backend_stream_apply_state(
                &o->graph_record_backend, o->graph_stream, op_index) != 0)
            return -1;
        if (op->kind == RSX_NIR_OP_DRAW &&
            (!o->graph_record_draw || o->graph_record_draw(
                 o->graph_prepare_user, &o->graph_record_backend,
                 o->graph_stream, op_index) != 0))
            return -1;
    }
    if (op->kind == RSX_NIR_OP_DRAW)
        o->graph_draws++;
    else if (rsx_nir_op_is_action(op->kind))
        o->graph_nondraw_actions++;
    if ((o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT
             ? rsx_nr_graph_op_ends_snapshot_island(op->kind)
             : rsx_nr_graph_op_ends_island(op->kind)) ||
        (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT &&
         o->graph_draws >= 64u))
        o->graph_boundary_after_method = 1u;
    return 0;
}

static u32 frame_graph_sink_side_push(void* user, const u32* words, u32 count)
{
    rsx_nr_frame_owner* const o = (rsx_nr_frame_owner*)user;
    return o && o->graph_stream
        ? rsx_nir_side_push(o->graph_stream, words, count) : ~0u;
}

void rsx_nr_frame_owner_set_single_pass_graph(
    rsx_nr_frame_owner* o, u32 mode, rsx_nir_stream* stream,
    rsx_nr_frame_now_ticks_fn now_ticks, void* clock_user,
    unsigned long long tick_frequency)
{
    if (!o)
        return;
    if (mode > RSX_NR_FRAME_GRAPH_SNAPSHOT ||
        (mode != RSX_NR_FRAME_GRAPH_DISABLED && !stream))
        mode = RSX_NR_FRAME_GRAPH_DISABLED;
    o->graph_mode = mode;
    o->graph_stream = mode == RSX_NR_FRAME_GRAPH_DISABLED ? NULL : stream;
    o->graph_now_ticks = now_ticks;
    o->graph_clock_user = clock_user;
    o->graph_tick_frequency = tick_frequency;
    o->graph_exec_pos = 0u;
    o->graph_execution_pending = 0u;
    o->graph_method_start_seen = o->adapter
        ? o->adapter->methods_seen : 0u;
    o->graph_boundary_after_method = 0u;
    o->graph_internal_active = 0u;
    o->graph_started_ticks = 0u;
    if (stream)
        rsx_nir_stream_reset(stream);
    if (o->adapter) {
        rsx_nir_sink sink = rsx_nr_ring_sink(o->ring);
        if (mode == RSX_NR_FRAME_GRAPH_EXECUTE ||
            mode == RSX_NR_FRAME_GRAPH_SNAPSHOT) {
            sink.user = o;
            sink.push = frame_graph_sink_push;
            sink.side_push = frame_graph_sink_side_push;
        }
        o->adapter->em.out = sink;
    }
}

void rsx_nr_frame_owner_set_snapshot_callbacks(
    rsx_nr_frame_owner* o, rsx_nr_frame_prepare_island_fn prepare,
    rsx_nr_frame_finish_island_fn finish, void* user)
{
    if (!o)
        return;
    o->graph_prepare_island = prepare;
    o->graph_finish_island = finish;
    o->graph_prepare_user = user;
}

void rsx_nr_frame_owner_set_snapshot_record_draw(
    rsx_nr_frame_owner* o, rsx_nr_frame_record_draw_fn record_draw)
{
    if (o)
        o->graph_record_draw = record_draw;
}

void rsx_nr_frame_owner_set_tail_clock(
    rsx_nr_frame_owner* o, rsx_nr_frame_now_ticks_fn now_ticks,
    void* clock_user, unsigned long long tick_frequency)
{
    if (!o)
        return;
    o->tail_now_ticks = now_ticks;
    o->tail_clock_user = clock_user;
    o->tail_tick_frequency = now_ticks ? tick_frequency : 0u;
}

void rsx_nr_frame_owner_set_tail_account(
    rsx_nr_frame_owner* o, rsx_nr_frame_tail_account_fn account,
    void* account_user)
{
    if (!o)
        return;
    o->tail_account = account;
    o->tail_account_user = account_user;
}

static void frame_publication_wait_start(rsx_nr_frame_owner* o)
{
    o->flow_wait_clock_next_poll = 0u;
    o->flow_wait_started_ms = 0u;
    o->flow_wait_cached_ms = 0u;
    o->flow_wait_next_proof_ms = 0u;
    if (o->publication_now_ms) {
        const unsigned long long now =
            o->publication_now_ms(o->publication_clock_user);
        o->flow_wait_started_ms = now;
        o->flow_wait_cached_ms = now;
        o->flow_wait_next_proof_ms = now +
            (o->publication_proof_delay_ms
                ? o->publication_proof_delay_ms
                : NR_FRAME_PUBLICATION_PROOF_DELAY_MS);
        o->flow_wait_clock_next_poll =
            NR_FRAME_PUBLICATION_CLOCK_POLL_INTERVAL;
    }
}

static void frame_publication_clock_refresh(rsx_nr_frame_owner* o)
{
    if (!o->publication_now_ms ||
        o->flow_wait_polls < o->flow_wait_clock_next_poll)
        return;
    o->flow_wait_cached_ms =
        o->publication_now_ms(o->publication_clock_user);
    o->flow_wait_clock_next_poll = o->flow_wait_polls +
        NR_FRAME_PUBLICATION_CLOCK_POLL_INTERVAL;
}

static int frame_publication_timed_out(const rsx_nr_frame_owner* o)
{
    if (!o->publication_now_ms)
        return o->flow_wait_polls > o->flow_wait_limit;
    const unsigned long long delay = o->publication_failure_delay_ms
        ? o->publication_failure_delay_ms
        : NR_FRAME_PUBLICATION_FAILURE_DELAY_MS;
    return o->flow_wait_cached_ms - o->flow_wait_started_ms >= delay;
}

static int frame_publication_proof_due(const rsx_nr_frame_owner* o)
{
    return o->publication_now_ms
        ? o->flow_wait_cached_ms >= o->flow_wait_next_proof_ms
        : o->flow_wait_put_polls >= NR_FRAME_GENERATED_LINK_PROOF_DELAY;
}

static void frame_publication_defer_proof(rsx_nr_frame_owner* o)
{
    if (o->publication_now_ms) {
        const unsigned long long delay = o->publication_proof_delay_ms
            ? o->publication_proof_delay_ms
            : NR_FRAME_PUBLICATION_PROOF_DELAY_MS;
        o->flow_wait_next_proof_ms = o->flow_wait_cached_ms + delay;
    } else {
        o->flow_wait_put_polls = 0u;
    }
}

static void frame_breadcrumb(rsx_nr_frame_owner* o, u32 get, u32 put,
                             u32 ret, u32 command)
{
    if (o->breadcrumb_count && get == o->breadcrumb_last_get &&
        ret == o->breadcrumb_last_return &&
        command == o->breadcrumb_last_command)
        return;
    rsx_nr_frame_breadcrumb* const crumb =
        &o->breadcrumbs[o->breadcrumb_head];
    crumb->get = get;
    crumb->put = put;
    crumb->call_return = ret;
    crumb->command = command;
    o->breadcrumb_head =
        (o->breadcrumb_head + 1u) % RSX_NR_FRAME_BREADCRUMB_COUNT;
    if (o->breadcrumb_count < RSX_NR_FRAME_BREADCRUMB_COUNT)
        o->breadcrumb_count++;
    o->breadcrumb_last_get = get;
    o->breadcrumb_last_return = ret;
    o->breadcrumb_last_command = command;
}

static int frame_previous_sequential_breadcrumb(
    const rsx_nr_frame_owner* o, u32 get, u32 ret,
    u32* previous_get, u32* previous_command)
{
    if (!o->breadcrumb_count || !previous_get || !previous_command)
        return 0;
    /* The newest breadcrumb is this unchanged wait cursor.  Walk backward to
     * the last distinct owner-observed command and accept only a complete
     * linear command ending exactly at GET. */
    for (u32 back = 2u; back <= o->breadcrumb_count; ++back) {
        const u32 index = (o->breadcrumb_head +
            RSX_NR_FRAME_BREADCRUMB_COUNT - back) %
            RSX_NR_FRAME_BREADCRUMB_COUNT;
        const rsx_nr_frame_breadcrumb* const crumb =
            &o->breadcrumbs[index];
        if (crumb->call_return != ret || crumb->get == get)
            continue;
        u32 bytes = 0u;
        if (crumb->command == 0u) {
            bytes = 4u;
        } else if ((crumb->command & 0xA0030003u) == 0u) {
            const u32 count = (crumb->command >> 18) & 0x7FFu;
            if (count)
                bytes = 4u + count * 4u;
        }
        if (bytes && frame_linear_next(crumb->get, bytes, ret) == get) {
            *previous_get = crumb->get;
            *previous_command = crumb->command;
            return 1;
        }
        return 0;
    }
    return 0;
}

static void frame_record_flow(rsx_nr_frame_owner* o, u32 get,
                              u32 command, u32 target,
                              u32 return_before, u32 return_after)
{
    o->flow_origin.get = get;
    o->flow_origin.command = command;
    o->flow_origin.target = target;
    o->flow_origin.return_before = return_before;
    o->flow_origin.return_after = return_after;
    o->flow_origin.sequence++;
}

static int frame_command_syntactically_ready(u32 command)
{
    if (command == 0u ||
        (command & 0xE0000003u) == 0x20000000u ||
        (command & 3u) == 1u || (command & 3u) == 2u ||
        (command & 0xFFFF0003u) == 0x00020000u)
        return 1;
    return (command & 0xA0030003u) == 0u &&
           ((command >> 18) & 0x7FFu) != 0u;
}

static int frame_command_is_jump(u32 command)
{
    return (command & 0xE0000003u) == 0x20000000u ||
           (command & 3u) == 1u;
}

static int frame_is_primary_segment_tail(const rsx_nr_frame_owner* o,
                                         u32 io, u32 ret)
{
    return ret == ~0u && io < NR_FRAME_RING_SIZE &&
           o->primary_segment_bytes &&
           (io + 4u) % o->primary_segment_bytes == 0u;
}

static int frame_is_generated_block_tail(const rsx_nr_frame_owner* o,
                                         u32 io, u32 ret)
{
    return ret == ~0u && io < NR_FRAME_RING_SIZE &&
           o->generated_block_bytes &&
           (io + 4u) % o->generated_block_bytes == 0u;
}

static int frame_flow_target_ready(const rsx_nr_frame_owner* o,
                                   u32 target, u32 ret, u32 command)
{
    if (!frame_command_syntactically_ready(command))
        return 0;
    /* The producer reserves the final word of every primary 1 MiB segment
     * exclusively for its link. Raw vertex/constant data can transiently
     * occupy the recycled word and can accidentally resemble a packet
     * header; only the published jump proves this dependency is ready. */
    if (frame_is_primary_segment_tail(o, target, ret) ||
        frame_is_generated_block_tail(o, target, ret))
        return frame_command_is_jump(command);
    return 1;
}

static rsx_nr_frame_step_result frame_wait_for_flow_target(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 ret, u32 command,
    u32 target, u32 target_word)
{
    if (o->flow_wait_source != get || o->flow_wait_target != target) {
        o->flow_wait_source = get;
        o->flow_wait_target = target;
        o->flow_wait_polls = 0u;
        o->flow_wait_put = put;
        o->flow_wait_put_polls = 0u;
        frame_publication_wait_start(o);
    } else if (o->flow_wait_put != put) {
        /* PUT is part of each proof snapshot, but unrelated command
         * publication is not progress on this exact dependency.  With the
         * production wall clock, retain the episode's original deadline and
         * rate-limit a new proof against the latest PUT.  Clockless offline
         * callers retain the older deterministic per-generation poll bound. */
        o->flow_wait_put = put;
        if (!o->publication_now_ms) {
            o->flow_wait_put_polls = 0u;
            o->flow_wait_polls = 0u;
        }
    }
    o->flow_wait_word = target_word;
    o->flow_wait_put_polls++;
    ++o->flow_wait_polls;
    frame_publication_clock_refresh(o);
    if (frame_publication_timed_out(o))
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, ret, command,
            target, target_word, o->flow_wait_polls);
    o->stats.waits_partial++;
    return RSX_NR_FRAME_WAIT_PARTIAL;
}

static rsx_nr_frame_step_result frame_wait_for_unsupported_candidate(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 ret, u32 command,
    u32 method, u32 argument)
{
    if (o->flow_wait_source != get || o->flow_wait_target != get) {
        o->flow_wait_source = get;
        o->flow_wait_target = get;
        o->flow_wait_polls = 0u;
        o->flow_wait_put = put;
        o->flow_wait_put_polls = 0u;
        frame_publication_wait_start(o);
    } else if (o->flow_wait_put != put) {
        o->flow_wait_put = put;
        if (!o->publication_now_ms) {
            o->flow_wait_put_polls = 0u;
            o->flow_wait_polls = 0u;
        }
    }
    o->flow_wait_word = command;
    o->flow_wait_put_polls++;
    ++o->flow_wait_polls;
    frame_publication_clock_refresh(o);
    if (frame_publication_timed_out(o))
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD, get, put, ret,
            command, method, argument, o->flow_wait_polls);
    o->stats.waits_partial++;
    return RSX_NR_FRAME_WAIT_PARTIAL;
}

static int frame_try_resolve_generated_jump(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 ret, u32 command,
    u32 target, u32 target_word, u32* next_get)
{
    u32 resume = 0u;
    u32 repaired = 0u;
    u32 resume_word = 0u;
    frame_publication_clock_refresh(o);
    const int same_attempt =
        o->repair_attempt_valid &&
        o->repair_attempt_kind == 1u &&
        o->repair_attempt_source == get &&
        o->repair_attempt_command == command &&
        o->repair_attempt_target == target &&
        o->repair_attempt_word == target_word &&
        o->repair_attempt_put == put;
    if (!o->resolve_jump || same_attempt ||
        o->flow_wait_source != get || o->flow_wait_target != target ||
        o->flow_wait_put != put || !frame_publication_proof_due(o))
        return 0;

    o->repair_attempt_valid = 1u;
    o->repair_attempt_kind = 1u;
    o->repair_attempt_source = get;
    o->repair_attempt_put = put;
    o->repair_attempt_command = command;
    o->repair_attempt_target = target;
    o->repair_attempt_word = target_word;
    o->stats.generated_link_attempts++;
    frame_publication_defer_proof(o);
    const int resolved = o->resolve_jump(
        o->resolve_jump_user, get, put, command, target,
        target_word, &resume);
    if (resolved <= 0) {
        if (resolved < 0) {
            /* The exact producer identity matched, but its dependent bytes
             * were not ready. PUT need not change when a generated target is
             * filled behind an already-published source edge. Do not rescan
             * on every poll: begin one new fixed proof-delay interval. */
            o->repair_attempt_valid = 0u;
            frame_publication_defer_proof(o);
        }
        return 0;
    }

    /* Success means the producer hook replaced the source JUMP. Re-read both
     * ends before advancing so a stale callback result cannot skip bytes. */
    if (resume == get || !frame_read(o, get, &repaired) ||
        !frame_read(o, resume, &resume_word) ||
        !frame_command_is_jump(repaired) ||
        ((repaired & 3u) == 1u
             ? (repaired & 0xFFFFFFFCu)
             : (repaired & 0x1FFFFFFCu)) != resume ||
        !frame_flow_target_ready(o, resume, ret, resume_word)) {
        /* Callback success is only the first half of the publication proof.
         * If the independent owner reread races the producer, this attempt is
         * pending rather than definitively refused.  Do not latch it forever
         * under an unchanged PUT; require another complete proof interval. */
        o->repair_attempt_valid = 0u;
        frame_publication_defer_proof(o);
        return 0;
    }

    o->repair_attempt_valid = 0u;
    o->flow_wait_source = ~0u;
    o->flow_wait_target = ~0u;
    o->flow_wait_polls = 0u;
    *next_get = resume;
    o->stats.repaired_generated_links++;
    frame_record_flow(o, get, repaired, resume, ret, ret);
    return 1;
}

static int frame_try_resolve_generated_hole(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 ret, u32 word,
    u32* next_get)
{
    u32 resume = 0u;
    u32 current = 0u;
    u32 resume_word = 0u;
    frame_publication_clock_refresh(o);
    const int same_attempt =
        o->repair_attempt_valid &&
        o->repair_attempt_kind == 2u &&
        o->repair_attempt_source == get &&
        o->repair_attempt_command == word &&
        o->repair_attempt_put == put;
    if (!o->resolve_hole || same_attempt || ret != ~0u ||
        o->flow_wait_source != get || o->flow_wait_target != get ||
        o->flow_wait_put != put || !frame_publication_proof_due(o))
        return 0;

    o->repair_attempt_valid = 1u;
    o->repair_attempt_kind = 2u;
    o->repair_attempt_source = get;
    o->repair_attempt_put = put;
    o->repair_attempt_command = word;
    o->repair_attempt_target = get;
    o->repair_attempt_word = word;
    o->stats.generated_link_attempts++;
    frame_publication_defer_proof(o);
    const int sequential_previous =
        !o->packet_active && o->packet_count &&
        o->packet_next_get == get && o->packet_next_ret == ret &&
        o->packet_get != get;
    const u32 previous_get = sequential_previous
        ? o->packet_get : ~0u;
    u32 previous_command = sequential_previous
        ? o->packet_command : 0u;
    u32 exact_previous_get = previous_get;
    if (!sequential_previous)
        frame_previous_sequential_breadcrumb(
            o, get, ret, &exact_previous_get, &previous_command);
    const int resolved = o->resolve_hole(
        o->resolve_hole_user, get, put, word,
        exact_previous_get, previous_command, &resume);
    if (resolved <= 0) {
        if (resolved < 0) {
            o->repair_attempt_valid = 0u;
            frame_publication_defer_proof(o);
        }
        return 0;
    }
    if (resume == get || !frame_read(o, get, &current) || current != word ||
        !frame_read(o, resume, &resume_word) ||
        !frame_flow_target_ready(o, resume, ret, resume_word)) {
        o->repair_attempt_valid = 0u;
        frame_publication_defer_proof(o);
        return 0;
    }

    o->repair_attempt_valid = 0u;
    o->flow_wait_source = ~0u;
    o->flow_wait_target = ~0u;
    o->flow_wait_polls = 0u;
    *next_get = resume;
    o->stats.repaired_generated_holes++;
    frame_record_flow(o, get, word, resume, ret, ret);
    return 1;
}

static void frame_graph_op_side(const rsx_nir_op* op, u32* offset,
                                u32* count)
{
    *offset = 0u;
    *count = 0u;
    switch (op->kind) {
    case RSX_NIR_OP_DRAW:
        *offset = op->u.draw.batches_ofs;
        *count = op->u.draw.batch_count * 2u;
        break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM:
        *offset = op->u.vertex_program.words_ofs;
        *count = op->u.vertex_program.word_count;
        break;
    case RSX_NIR_OP_SET_CONSTANTS:
        *offset = op->u.constants.words_ofs;
        *count = op->u.constants.slot_count * 4u;
        break;
    case RSX_NIR_OP_TRANSFER:
        *offset = op->u.transfer.words_ofs;
        *count = op->u.transfer.word_count;
        break;
    default:
        break;
    }
}

static void frame_graph_set_side(rsx_nir_op* op, u32 offset)
{
    switch (op->kind) {
    case RSX_NIR_OP_DRAW:
        op->u.draw.batches_ofs = offset;
        break;
    case RSX_NIR_OP_SET_VERTEX_PROGRAM:
        op->u.vertex_program.words_ofs = offset;
        break;
    case RSX_NIR_OP_SET_CONSTANTS:
        op->u.constants.words_ofs = offset;
        break;
    case RSX_NIR_OP_TRANSFER:
        op->u.transfer.words_ofs = offset;
        break;
    default:
        break;
    }
}

static unsigned long long frame_graph_hash_bytes(
    unsigned long long hash, const void* data, size_t size)
{
    const unsigned char* p = (const unsigned char*)data;
    if (!hash)
        hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static unsigned long long frame_graph_hash_op(
    unsigned long long hash, const rsx_nir_op* source,
    const u32* side, u32 side_count)
{
    rsx_nir_op op = *source;
    u32 offset = 0u, count = 0u;
    frame_graph_op_side(&op, &offset, &count);
    frame_graph_set_side(&op, 0u);
    hash = frame_graph_hash_bytes(hash, &op, sizeof(op));
    if (count && side && offset <= side_count && count <= side_count - offset)
        hash = frame_graph_hash_bytes(
            hash, side + offset, (size_t)count * sizeof(u32));
    return hash;
}

static void frame_graph_start(rsx_nr_frame_owner* o)
{
    if (!o->graph_started_ticks && o->graph_now_ticks)
        o->graph_started_ticks =
            o->graph_now_ticks(o->graph_clock_user);
}

static void frame_graph_close_construction(rsx_nr_frame_owner* o)
{
    if (o->graph_started_ticks && o->graph_now_ticks) {
        const unsigned long long end =
            o->graph_now_ticks(o->graph_clock_user);
        if (end >= o->graph_started_ticks)
            o->graph_stats.construction_ticks +=
                end - o->graph_started_ticks;
    }
    o->graph_started_ticks = 0u;
}

static void frame_graph_reset_island(rsx_nr_frame_owner* o)
{
    rsx_nir_stream_reset(o->graph_stream);
    o->graph_exec_pos = 0u;
    o->graph_execution_pending = 0u;
    o->graph_method_start_seen = o->adapter
        ? o->adapter->methods_seen : 0u;
    o->graph_boundary_after_method = 0u;
    o->graph_passive_source_ops = 0u;
    o->graph_passive_source_side = 0u;
    o->graph_draws = 0u;
    o->graph_nondraw_actions = 0u;
    o->graph_prepared = 0u;
    o->graph_record_initialized = 0u;
    o->graph_passive_source_hash = 0u;
    o->graph_started_ticks = 0u;
}

static void frame_graph_account_island(rsx_nr_frame_owner* o, int passive)
{
    const u32 methods = o->adapter
        ? o->adapter->methods_seen - o->graph_method_start_seen : 0u;
    const u32 ops = o->graph_stream->op_count;
    const u32 side = o->graph_stream->side_count;
    o->graph_stats.islands++;
    o->graph_stats.methods += methods;
    o->graph_stats.ops += ops;
    o->graph_stats.side_words += side;
    if (methods > o->graph_stats.max_methods)
        o->graph_stats.max_methods = methods;
    if (ops > o->graph_stats.max_ops)
        o->graph_stats.max_ops = ops;
    if (side > o->graph_stats.max_side_words)
        o->graph_stats.max_side_words = side;
    if (passive)
        o->graph_stats.passive_islands++;
}

static void frame_graph_passive_finish(rsx_nr_frame_owner* o)
{
    if (!o->graph_stream || !o->graph_stream->op_count)
        return;
    unsigned long long hash = 0u;
    for (u32 i = 0; i < o->graph_stream->op_count; ++i)
        hash = frame_graph_hash_op(
            hash, &o->graph_stream->ops[i], o->graph_stream->side,
            o->graph_stream->side_count);
    if (o->graph_passive_source_ops == o->graph_stream->op_count &&
        o->graph_passive_source_side == o->graph_stream->side_count &&
        o->graph_passive_source_hash == hash)
        o->graph_stats.passive_equivalent++;
    else
        o->graph_stats.passive_mismatches++;
    frame_graph_account_island(o, 1);
    frame_graph_reset_island(o);
}

static int frame_graph_passive_record(rsx_nr_frame_owner* o,
                                      const rsx_nr_slot* slot)
{
    rsx_nir_op op = slot->op;
    u32 offset = 0u, count = 0u;
    frame_graph_op_side(&op, &offset, &count);
    if (o->graph_stream->op_count == o->graph_stream->op_cap ||
        count > o->graph_stream->side_cap - o->graph_stream->side_count) {
        frame_graph_passive_finish(o);
        o->graph_stats.fallback[RSX_NR_FRAME_GRAPH_FB_CAPACITY]++;
    }
    const u32* source = count
        ? rsx_nr_ring_side_ptr(o->ring, offset) : NULL;
    const u32 stream_offset = count
        ? rsx_nir_side_push(o->graph_stream, source, count) : 0u;
    if ((count && stream_offset == ~0u) ||
        rsx_nir_push(o->graph_stream, &op) != 0)
        return -1;
    frame_graph_set_side(
        &o->graph_stream->ops[o->graph_stream->op_count - 1u],
        stream_offset);
    o->graph_passive_source_hash = frame_graph_hash_op(
        o->graph_passive_source_hash, &slot->op, o->ring->side,
        o->ring->side_cap);
    o->graph_passive_source_ops++;
    o->graph_passive_source_side += count;
    if (op.kind == RSX_NIR_OP_PRESENT)
        o->graph_stats.frames++;
    if (rsx_nr_graph_op_ends_island(op.kind))
        o->graph_boundary_after_method = 1u;
    return 0;
}

static rsx_nr_frame_step_result frame_drain_method(rsx_nr_frame_owner* o)
{
    while (rsx_nr_ring_depth(o->ring)) {
        const rsx_nr_slot* const slot = rsx_nr_ring_peek(o->ring);
        const u32 graph_ops_before = o->graph_stream
            ? o->graph_stream->op_count : 0u;
        const u32 graph_side_before = o->graph_stream
            ? o->graph_stream->side_count : 0u;
        const u32 graph_source_ops_before = o->graph_passive_source_ops;
        const u32 graph_source_side_before = o->graph_passive_source_side;
        const unsigned long long graph_hash_before =
            o->graph_passive_source_hash;
        if (o->graph_mode == RSX_NR_FRAME_GRAPH_PASSIVE &&
            (!slot || frame_graph_passive_record(o, slot) != 0))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command,
                o->packet_method, o->packet_argument, o->packet_index);
        const unsigned long long ops_before =
            o->backend->stats.executed[RSX_NIR_OP_PRESENT];
        const rsx_nr_step_result result = rsx_nr_backend_step(o->backend);
        if (result == RSX_NR_STEP_BLOCKED_SEMAPHORE ||
            result == RSX_NR_STEP_BLOCKED_TOKEN) {
            if (o->graph_mode == RSX_NR_FRAME_GRAPH_PASSIVE) {
                o->graph_stream->op_count = graph_ops_before;
                o->graph_stream->side_count = graph_side_before;
                o->graph_passive_source_ops = graph_source_ops_before;
                o->graph_passive_source_side = graph_source_side_before;
                o->graph_passive_source_hash = graph_hash_before;
            }
            o->stats.waits_semaphore++;
            return RSX_NR_FRAME_WAIT_SEMAPHORE;
        }
        if (result != RSX_NR_STEP_EXECUTED ||
            o->backend->stats.exec_errors != o->method_errors_before)
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_EXECUTION, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command,
                o->packet_non_incrementing ? o->packet_method
                    : o->packet_method + o->packet_index * 4u,
                o->packet_argument, o->packet_index);
        o->stats.backend_ops++;
        if (o->backend->stats.executed[RSX_NIR_OP_PRESENT] != ops_before)
            o->stats.frames++;
    }
    o->method_inflight = 0;
    o->packet_index++;
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_PASSIVE &&
        o->graph_boundary_after_method)
        frame_graph_passive_finish(o);
    return RSX_NR_FRAME_ADVANCED;
}

static rsx_nr_frame_step_result frame_resume_packet(
    rsx_nr_frame_owner* o, u32 get, u32 call_return,
    u32* next_get, u32* next_return)
{
    if (get != o->packet_get || call_return != o->packet_ret)
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_CURSOR_CHANGED, get, o->packet_put,
            call_return, o->packet_command, o->packet_method, 0u,
            o->packet_index);

    for (;;) {
        if (o->method_inflight) {
            const rsx_nr_frame_step_result drained = frame_drain_method(o);
            if (drained != RSX_NR_FRAME_ADVANCED)
                return drained;
        }
        if (o->packet_index == o->packet_count) {
            *next_get = o->packet_next_get;
            *next_return = o->packet_next_ret;
            o->packet_active = 0;
            o->stats.packets++;
            if (o->graph_mode == RSX_NR_FRAME_GRAPH_EXECUTE)
                o->graph_yield_after_packet = 1u;
            return RSX_NR_FRAME_ADVANCED;
        }

        const u32 method = o->packet_non_incrementing
            ? o->packet_method : o->packet_method + o->packet_index * 4u;
        const rsx_nr_graph_method_boundary graph_boundary =
            o->graph_mode != RSX_NR_FRAME_GRAPH_DISABLED &&
                    o->graph_stream->op_count
                ? rsx_nr_graph_classify_method(method)
                : RSX_NR_GRAPH_METHOD_CONTINUE;
        if (frame_graph_exec_mode(o) &&
            o->graph_stream->op_count &&
            graph_boundary != RSX_NR_GRAPH_METHOD_CONTINUE) {
            frame_graph_close_construction(o);
            return RSX_NR_FRAME_GRAPH_BOUNDARY;
        }
        if (o->graph_mode == RSX_NR_FRAME_GRAPH_PASSIVE &&
            o->graph_stream->op_count &&
            graph_boundary != RSX_NR_GRAPH_METHOD_CONTINUE)
            frame_graph_passive_finish(o);
        u32 argument = 0;
        const u32 argument_io = o->packet_get + 4u + o->packet_index * 4u;
        if (!frame_read(o, argument_io, &argument))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_UNMAPPED, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method, 0u,
                o->packet_index);
        if (!rsx_nir_adapter_method_supported(o->adapter, method, argument))
        {
            if (o->graph_mode != RSX_NR_FRAME_GRAPH_DISABLED)
                o->graph_stats.fallback[
                    RSX_NR_FRAME_GRAPH_FB_UNSUPPORTED_METHOD]++;
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);
        }
        if (frame_graph_exec_mode(o) &&
            (o->graph_stream->op_count + NR_FRAME_METHOD_OP_BOUND >
                 o->graph_stream->op_cap ||
             o->graph_stream->side_count + NR_FRAME_METHOD_SIDE_BOUND >
                 o->graph_stream->side_cap)) {
            if (o->graph_stream->op_count) {
                o->graph_stats.fallback[
                    RSX_NR_FRAME_GRAPH_FB_CAPACITY]++;
                frame_graph_close_construction(o);
                return RSX_NR_FRAME_GRAPH_BOUNDARY;
            }
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);
        }
        if (!frame_graph_exec_mode(o) &&
            !rsx_nr_ring_can_accept(
                o->ring, NR_FRAME_METHOD_OP_BOUND,
                NR_FRAME_METHOD_SIDE_BOUND))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);

        if (frame_graph_exec_mode(o))
            frame_graph_start(o);
        else
            rsx_nr_ring_clear_reject(o->ring);
        o->method_errors_before = o->backend->stats.exec_errors;
        o->packet_argument = argument;
        const unsigned long long adaptation_started = o->tail_now_ticks
            ? o->tail_now_ticks(o->tail_clock_user) : 0u;
        rsx_nir_adapter_method(o->adapter, method, argument);
        if (adaptation_started && o->tail_now_ticks) {
            const unsigned long long adaptation_ended =
                o->tail_now_ticks(o->tail_clock_user);
            if (adaptation_ended >= adaptation_started) {
                const unsigned long long elapsed =
                    adaptation_ended - adaptation_started;
                o->stats.adaptation_calls++;
                o->stats.adaptation_ticks += elapsed;
                if (o->tail_account)
                    o->tail_account(o->tail_account_user, elapsed);
            }
        }
        o->stats.methods++;
        if (frame_graph_exec_mode(o) &&
            (o->graph_stream->overflow || o->graph_stream->oom))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);
        if (!frame_graph_exec_mode(o) &&
            rsx_nr_ring_reject_sticky(o->ring))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);
        if (frame_graph_exec_mode(o)) {
            o->packet_index++;
            if (o->graph_boundary_after_method) {
                frame_graph_close_construction(o);
                return RSX_NR_FRAME_GRAPH_BOUNDARY;
            }
            continue;
        }
        o->method_inflight = rsx_nr_ring_depth(o->ring) != 0u;
        if (!o->method_inflight) {
            o->packet_index++;
            continue;
        }
    }
}

static rsx_nr_frame_step_result frame_owner_step_once(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return)
{
    u32 command = 0;
    if (!o || !o->adapter || !o->backend || !o->ring || !next_get ||
        !next_return)
        return RSX_NR_FRAME_FATAL;
    *next_get = get;
    *next_return = call_return;
    o->stats.steps++;
    if (o->fatal)
        return RSX_NR_FRAME_FATAL;
    if (o->packet_active)
        return frame_resume_packet(
            o, get, call_return, next_get, next_return);
    if (get == put) {
        o->stats.waits_empty++;
        return RSX_NR_FRAME_WAIT_EMPTY;
    }
    if (rsx_nr_fifo_section_range_status(
            get, 4u, put, call_return, NR_FRAME_RING_SIZE) !=
        RSX_NR_FIFO_RANGE_READY) {
        o->stats.waits_partial++;
        return RSX_NR_FRAME_WAIT_PARTIAL;
    }
    if (!frame_read(o, get, &command))
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_UNMAPPED, get, put, call_return,
            0u, 0u, 0u, 0u);
    frame_breadcrumb(o, get, put, call_return, command);
    if (frame_is_primary_segment_tail(o, get, call_return) &&
        !frame_command_is_jump(command))
        return frame_wait_for_flow_target(
            o, get, put, call_return, command, get, command);
    if (command == 0u) {
        *next_get = frame_linear_next(get, 4u, call_return);
        return frame_control_advance(o, get, put, call_return, command);
    }
    if ((command & 0xE0000003u) == 0x20000000u ||
        (command & 3u) == 1u) {
        const u32 target = (command & 3u) == 1u
            ? (command & 0xFFFFFFFCu) : (command & 0x1FFFFFFCu);
        u32 target_word = 0;
        u32 island_resume = 0;
        const int island_result = o->island_edge
            ? o->island_edge(o->island_edge_user, get, put, command,
                             &island_resume)
            : 0;
        if (island_result) {
            if (island_resume == get ||
                !frame_read(o, island_resume, &target_word))
                return frame_fail(
                    o, RSX_NR_FRAME_FAILURE_ISLAND_EDGE, get, put,
                    call_return, command, island_resume, target, 0u);
            o->flow_wait_source = ~0u;
            o->flow_wait_target = ~0u;
            o->flow_wait_polls = 0u;
            *next_get = island_resume;
            o->stats.skipped_data_islands++;
            if (island_result == 2)
                o->stats.recovered_late_island_entries++;
            frame_record_flow(
                o, get, command, island_resume, call_return,
                call_return);
            return frame_control_advance(
                o, get, put, call_return, command);
        }
        if (target == get) {
            u32 resume = get;
            if (o->release_stopper && o->release_stopper(
                    o->release_stopper_user, get, put, command,
                    &resume)) {
                if (resume == get || !frame_read(o, resume, &target_word))
                    return frame_fail(
                        o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put,
                        call_return, command, 0u, resume, 0u);
                *next_get = resume;
                o->stats.released_stoppers++;
                frame_record_flow(
                    o, get, command, resume, call_return, call_return);
                return frame_control_advance(
                    o, get, put, call_return, command);
            }
            o->stats.waits_stopper++;
            return RSX_NR_FRAME_WAIT_STOPPER;
        }
        if (!frame_read(o, target, &target_word)) {
            /* A producer may be replacing recycled payload in place when its
             * low bits transiently resemble an absolute JUMP.  An unmapped
             * target from the primary ring is not executable evidence yet;
             * retain the exact source word in the ordinary bounded
             * publication wait. Once its proof delay expires, also give the
             * exact generated-hole oracle the preceding-command breadcrumb:
             * producer-owned float payload can encode an out-of-range JUMP,
             * but only a bounded NOOP/prologue/chain proof may bypass it.
             * Called lists remain strict failures. */
            if (call_return == ~0u && get < NR_FRAME_RING_SIZE &&
                frame_try_resolve_generated_hole(
                    o, get, put, call_return, command, next_get))
                return frame_control_advance(
                    o, get, put, call_return, command);
            if (call_return == ~0u && get < NR_FRAME_RING_SIZE)
                return frame_wait_for_flow_target(
                    o, get, put, call_return, command, get, command);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
                command, 0u, target, 0u);
        }
        int target_ready = frame_flow_target_ready(
            o, target, call_return, target_word);
        if (!target_ready &&
            frame_is_generated_block_tail(o, target, call_return) &&
            frame_command_syntactically_ready(target_word) &&
            o->released_edge && o->released_edge(
                o->released_edge_user, get, put, command,
                target, target_word)) {
            /* The exact fenced producer edge distinguishes a real packet
             * beginning at this address from recycled command-shaped data.
             * The target itself is consumed normally on the next step. */
            target_ready = 1;
            o->stats.admitted_released_boundaries++;
        }
        if (!target_ready) {
            if (call_return == ~0u && target < NR_FRAME_RING_SIZE &&
                frame_try_resolve_generated_jump(
                    o, get, put, call_return, command, target,
                    target_word, next_get))
                return frame_control_advance(
                    o, get, put, call_return, command);
            if (call_return == ~0u && target < NR_FRAME_RING_SIZE)
                return frame_wait_for_flow_target(
                    o, get, put, call_return, command, target, target_word);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
                command, target, target_word, 0u);
        }
        o->flow_wait_source = ~0u;
        o->flow_wait_target = ~0u;
        o->flow_wait_polls = 0u;
        *next_get = target;
        frame_record_flow(
            o, get, command, target, call_return, call_return);
        return frame_control_advance(o, get, put, call_return, command);
    }
    if ((command & 3u) == 2u) {
        const u32 target = command & 0x1FFFFFFCu;
        u32 target_word = 0;
        /* GCM has a single return slot: CALL while already inside a called
         * list can never be a valid command.  Live producers publish the
         * caller edge before all target bytes have necessarily become
         * visible, and raw target data can transiently decode as CALL.
         * Keep that exact cursor in one bounded publication episode. */
        if (call_return != ~0u)
            return frame_wait_for_flow_target(
                o, get, put, call_return, command, get, command);
        if (!frame_read(o, target, &target_word)) {
            /* Raw generated-program tail data can alias an absolute CALL to
             * an unmapped address. As with an unmapped JUMP-shaped payload,
             * only the delayed exact producer-boundary oracle may advance it;
             * genuine mapped CALLs and called-list nesting remain strict. */
            if (get < NR_FRAME_RING_SIZE &&
                frame_try_resolve_generated_hole(
                    o, get, put, call_return, command, next_get))
                return frame_control_advance(
                    o, get, put, call_return, command);
            if (call_return == ~0u && get < NR_FRAME_RING_SIZE)
                return frame_wait_for_flow_target(
                    o, get, put, call_return, command, get, command);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
                command, 0u, target, 0u);
        }
        if (!frame_flow_target_ready(
                o, target, call_return, target_word)) {
            if (call_return == ~0u && target < NR_FRAME_RING_SIZE)
                return frame_wait_for_flow_target(
                    o, get, put, call_return, command, target, target_word);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
                command, target, target_word, 0u);
        }
        o->flow_wait_source = ~0u;
        o->flow_wait_target = ~0u;
        o->flow_wait_polls = 0u;
        *next_return = get < NR_FRAME_RING_SIZE
            ? ((get + 4u) & NR_FRAME_RING_MASK) : get + 4u;
        *next_get = target;
        frame_record_flow(
            o, get, command, target, call_return, *next_return);
        return frame_control_advance(o, get, put, call_return, command);
    }
    if ((command & 0xFFFF0003u) == 0x00020000u) {
        if (call_return == ~0u)
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
                command, 0u, 0u, 0u);
        *next_get = call_return;
        *next_return = ~0u;
        frame_record_flow(
            o, get, command, call_return, call_return, ~0u);
        return frame_control_advance(o, get, put, call_return, command);
    }
    if (!frame_command_syntactically_ready(command)) {
        if (frame_try_resolve_generated_hole(
                o, get, put, call_return, command, next_get))
            return frame_control_advance(
                o, get, put, call_return, command);
        if (call_return == ~0u && get < NR_FRAME_RING_SIZE)
            return frame_wait_for_flow_target(
                o, get, put, call_return, command, get, command);
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
            command, get, command, 0u);
    }
    const u32 count = (command >> 18) & 0x7FFu;
    if (!count)
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
            command, 0u, 0u, 0u);
    const u32 bytes = 4u + count * 4u;
    if (rsx_nr_fifo_section_range_status(
            get, bytes, put, call_return, NR_FRAME_RING_SIZE) !=
        RSX_NR_FIFO_RANGE_READY) {
        o->stats.waits_partial++;
        return RSX_NR_FRAME_WAIT_PARTIAL;
    }

    /* Recycled generated data can satisfy the packet-header mask by chance.
     * If its first would-be method is unsupported, do not activate or partly
     * execute the packet. Give the exact generated-hole oracle one bounded,
     * latched opportunity to prove the following command prologue. A genuine
     * unsupported method remains an exact development failure at the bound. */
    if (o->resolve_hole && call_return == ~0u && get < NR_FRAME_RING_SIZE) {
        const u32 method = command & 0x3FFFCu;
        u32 first_argument = 0u;
        if (!frame_read(o, get + 4u, &first_argument))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_UNMAPPED, get, put, call_return,
                command, method, 0u, 0u);
        if (!rsx_nir_adapter_method_supported(
                o->adapter, method, first_argument)) {
            if (frame_try_resolve_generated_hole(
                    o, get, put, call_return, command, next_get))
                return frame_control_advance(
                    o, get, put, call_return, command);
            return frame_wait_for_unsupported_candidate(
                o, get, put, call_return, command, method,
                first_argument);
        }
    }

    o->flow_wait_source = ~0u;
    o->flow_wait_target = ~0u;
    o->flow_wait_polls = 0u;

    o->packet_active = 1;
    o->control_streak = 0;
    o->packet_get = get;
    o->packet_put = put;
    o->packet_ret = call_return;
    o->packet_command = command;
    o->packet_count = count;
    o->packet_index = 0;
    o->packet_method = command & 0x3FFFCu;
    o->packet_non_incrementing = (command & 0x40000000u) != 0u;
    o->packet_next_get = frame_linear_next(get, bytes, call_return);
    o->packet_next_ret = call_return;
    return frame_resume_packet(o, get, call_return, next_get, next_return);
}

static int frame_graph_all_draws_snapshotted(const rsx_nir_stream* stream)
{
    if (!stream)
        return 0;
    for (u32 i = 0; i < stream->op_count; ++i) {
        const rsx_nir_op* const op = &stream->ops[i];
        if (op->kind == RSX_NIR_OP_DRAW && !op->u.draw.snapshot_id)
            return 0;
    }
    return 1;
}

static rsx_nr_frame_step_result frame_graph_execute_island(
    rsx_nr_frame_owner* o)
{
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT &&
        !o->graph_prepared) {
        const int prepared = o->graph_prepare_island
            ? o->graph_prepare_island(
                  o->graph_prepare_user, o->backend,
                  o->graph_stream)
            : -1;
        if (prepared < 0) {
            o->graph_stats.fallback[RSX_NR_FRAME_GRAPH_FB_EXECUTION]++;
            if (o->graph_finish_island)
                o->graph_finish_island(
                    o->graph_prepare_user, o->backend, 0);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_EXECUTION, o->graph_cursor_get,
                o->packet_put, o->graph_cursor_ret, o->packet_command,
                o->packet_method, o->packet_argument, o->packet_index);
        }
        o->graph_prepared = prepared >= 0 && o->graph_draws &&
                !o->graph_nondraw_actions &&
                frame_graph_all_draws_snapshotted(o->graph_stream)
            ? 2u : 1u;
    }
    const unsigned long long started = o->graph_now_ticks
        ? o->graph_now_ticks(o->graph_clock_user) : 0u;
    while (o->graph_exec_pos < o->graph_stream->op_count) {
        const rsx_nir_op* const op =
            &o->graph_stream->ops[o->graph_exec_pos];
        if (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT &&
            o->graph_prepared == 2u &&
            !rsx_nir_op_is_action(op->kind)) {
            if (op->kind < RSX_NIR_OP_KIND_COUNT)
                o->backend->stats.executed[op->kind]++;
            o->stats.backend_ops++;
            o->graph_exec_pos++;
            continue;
        }
        const unsigned long long errors_before =
            o->backend->stats.exec_errors;
        const rsx_nr_step_result result = rsx_nr_backend_stream_step(
            o->backend, o->graph_stream, o->graph_exec_pos);
        if (result == RSX_NR_STEP_BLOCKED_SEMAPHORE ||
            result == RSX_NR_STEP_BLOCKED_TOKEN) {
            if (started && o->graph_now_ticks) {
                const unsigned long long ended =
                    o->graph_now_ticks(o->graph_clock_user);
                if (ended >= started)
                    o->graph_stats.execution_ticks += ended - started;
            }
            o->stats.waits_semaphore++;
            return RSX_NR_FRAME_WAIT_SEMAPHORE;
        }
        if (result != RSX_NR_STEP_EXECUTED ||
            o->backend->stats.exec_errors != errors_before) {
            if (started && o->graph_now_ticks) {
                const unsigned long long ended =
                    o->graph_now_ticks(o->graph_clock_user);
                if (ended >= started)
                    o->graph_stats.execution_ticks += ended - started;
            }
            o->graph_stats.fallback[RSX_NR_FRAME_GRAPH_FB_EXECUTION]++;
            if (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT &&
                o->graph_finish_island)
                o->graph_finish_island(
                    o->graph_prepare_user, o->backend, 0);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_EXECUTION, o->graph_cursor_get,
                o->packet_put, o->graph_cursor_ret, o->packet_command,
                o->packet_method, o->packet_argument, o->packet_index);
        }
        o->stats.backend_ops++;
        if (op->kind == RSX_NIR_OP_PRESENT) {
            o->stats.frames++;
            o->graph_stats.frames++;
        }
        o->graph_exec_pos++;
    }
    if (started && o->graph_now_ticks) {
        const unsigned long long ended =
            o->graph_now_ticks(o->graph_clock_user);
        if (ended >= started) {
            o->graph_stats.execution_ticks += ended - started;
        }
    }
    if (started)
        o->graph_stats.timed_islands++;
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT &&
        o->graph_finish_island)
        o->graph_finish_island(
            o->graph_prepare_user, o->backend, 1);
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT &&
        o->graph_prepared == 2u && o->graph_record_initialized) {
        o->backend->st = o->graph_record_backend.st;
        o->backend->vp_word_count = o->graph_record_backend.vp_word_count;
        if (o->backend->vp_word_count)
            memcpy(o->backend->vp_words, o->graph_record_backend.vp_words,
                   (size_t)o->backend->vp_word_count * sizeof(u32));
    }
    frame_graph_account_island(o, 0);
    frame_graph_reset_island(o);
    /* The island is now atomically committed.  The packet-completion path
     * also requests a host handoff as soon as each complete packet has been
     * copied into the fixed graph arena.  Neither handoff forces a D3D
     * submission. */
    o->graph_yield_after_packet = 1u;
    return RSX_NR_FRAME_ADVANCED;
}

static rsx_nr_frame_step_result frame_graph_step(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return)
{
    if (!o->graph_internal_active) {
        o->graph_internal_active = 1u;
        o->graph_cursor_get = get;
        o->graph_cursor_ret = call_return;
        o->graph_external_get = get;
        o->graph_external_ret = call_return;
    } else if (get != o->graph_external_get ||
               call_return != o->graph_external_ret) {
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_CURSOR_CHANGED, get, put,
            call_return, 0u, 0u, 0u, 0u);
    }

    *next_get = get;
    *next_return = call_return;
    if (o->graph_now_ticks)
        o->graph_stats.calls++;

    for (u32 guard = 0; guard < NR_FRAME_FLOW_WAIT_BOUND; ++guard) {
        if (o->graph_execution_pending) {
            const rsx_nr_frame_step_result executed =
                frame_graph_execute_island(o);
            if (executed != RSX_NR_FRAME_ADVANCED)
                return executed;
        }

        u32 cursor_get = o->graph_cursor_get;
        u32 cursor_ret = o->graph_cursor_ret;
        const rsx_nr_frame_step_result result = frame_owner_step_once(
            o, o->graph_cursor_get, put, o->graph_cursor_ret,
            &cursor_get, &cursor_ret);
        if (result == RSX_NR_FRAME_GRAPH_BOUNDARY) {
            o->graph_execution_pending = 1u;
            continue;
        }
        if (result == RSX_NR_FRAME_ADVANCED) {
            o->graph_cursor_get = cursor_get;
            o->graph_cursor_ret = cursor_ret;
            if (o->graph_yield_after_packet && !o->packet_active) {
                o->graph_yield_after_packet = 0u;
                *next_get = cursor_get;
                *next_return = cursor_ret;
                o->graph_internal_active = 0u;
                return RSX_NR_FRAME_ADVANCED;
            }
            continue;
        }
        if (result == RSX_NR_FRAME_WAIT_EMPTY ||
            result == RSX_NR_FRAME_WAIT_PARTIAL ||
            result == RSX_NR_FRAME_WAIT_STOPPER) {
            if (o->graph_stream->op_count) {
                frame_graph_close_construction(o);
                o->graph_execution_pending = 1u;
                const rsx_nr_frame_step_result executed =
                    frame_graph_execute_island(o);
                if (executed != RSX_NR_FRAME_ADVANCED)
                    return executed;
            }
            if (o->graph_cursor_get != get ||
                o->graph_cursor_ret != call_return) {
                *next_get = o->graph_cursor_get;
                *next_return = o->graph_cursor_ret;
                o->graph_internal_active = 0u;
                return RSX_NR_FRAME_ADVANCED;
            }
            o->graph_internal_active = 0u;
            return result;
        }
        return result;
    }
    return frame_fail(
        o, RSX_NR_FRAME_FAILURE_BAD_FLOW, o->graph_cursor_get, put,
        o->graph_cursor_ret, 0u, 0u, 0u, NR_FRAME_FLOW_WAIT_BOUND);
}

rsx_nr_frame_step_result rsx_nr_frame_owner_step(
    rsx_nr_frame_owner* o, u32 get, u32 put, u32 call_return,
    u32* next_get, u32* next_return)
{
    if (!o || !next_get || !next_return)
        return RSX_NR_FRAME_FATAL;
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_EXECUTE ||
        o->graph_mode == RSX_NR_FRAME_GRAPH_SNAPSHOT)
        return frame_graph_step(
            o, get, put, call_return, next_get, next_return);
    const rsx_nr_frame_step_result result = frame_owner_step_once(
        o, get, put, call_return, next_get, next_return);
    if (o->graph_mode == RSX_NR_FRAME_GRAPH_PASSIVE &&
        !o->packet_active &&
        (result == RSX_NR_FRAME_WAIT_EMPTY ||
         result == RSX_NR_FRAME_WAIT_PARTIAL ||
         result == RSX_NR_FRAME_WAIT_STOPPER))
        frame_graph_passive_finish(o);
    return result;
}
