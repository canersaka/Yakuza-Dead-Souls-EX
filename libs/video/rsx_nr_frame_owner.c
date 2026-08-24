/* Strict native RSX frame owner. See rsx_nr_frame_owner.h. */

#include "rsx_nr_frame_owner.h"

#include <string.h>

#define NR_FRAME_RING_SIZE 0x800000u
#define NR_FRAME_RING_MASK (NR_FRAME_RING_SIZE - 1u)
#define NR_FRAME_METHOD_OP_BOUND 512u
#define NR_FRAME_METHOD_SIDE_BOUND 16384u
#define NR_FRAME_CONTROL_BOUND 4096u
#define NR_FRAME_FLOW_WAIT_BOUND (1u << 24)
#define NR_FRAME_GENERATED_LINK_PROOF_DELAY (1u << 16)
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
    } else if (o->flow_wait_put != put) {
        /* PUT is the producer publication generation for this dependency.
         * A proof made against an earlier snapshot cannot permanently latch
         * out data published later.  Require the new generation itself to
         * remain stable for the full proof delay before rescanning it.  A
         * moving PUT is also concrete producer progress, so the fatal bound
         * belongs to the new unchanged generation rather than accumulating
         * across every publication made while this target is being built. */
        o->flow_wait_put = put;
        o->flow_wait_put_polls = 0u;
        o->flow_wait_polls = 0u;
    }
    o->flow_wait_word = target_word;
    o->flow_wait_put_polls++;
    if (++o->flow_wait_polls > o->flow_wait_limit)
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
    } else if (o->flow_wait_put != put) {
        o->flow_wait_put = put;
        o->flow_wait_put_polls = 0u;
        o->flow_wait_polls = 0u;
    }
    o->flow_wait_word = command;
    o->flow_wait_put_polls++;
    if (++o->flow_wait_polls > o->flow_wait_limit)
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
        o->flow_wait_put != put ||
        o->flow_wait_put_polls < NR_FRAME_GENERATED_LINK_PROOF_DELAY)
        return 0;

    o->repair_attempt_valid = 1u;
    o->repair_attempt_kind = 1u;
    o->repair_attempt_source = get;
    o->repair_attempt_put = put;
    o->repair_attempt_command = command;
    o->repair_attempt_target = target;
    o->repair_attempt_word = target_word;
    o->stats.generated_link_attempts++;
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
            o->flow_wait_put_polls = 0u;
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
        o->flow_wait_put_polls = 0u;
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
    const int same_attempt =
        o->repair_attempt_valid &&
        o->repair_attempt_kind == 2u &&
        o->repair_attempt_source == get &&
        o->repair_attempt_command == word &&
        o->repair_attempt_put == put;
    if (!o->resolve_hole || same_attempt || ret != ~0u ||
        o->flow_wait_source != get || o->flow_wait_target != get ||
        o->flow_wait_put != put ||
        o->flow_wait_put_polls < NR_FRAME_GENERATED_LINK_PROOF_DELAY)
        return 0;

    o->repair_attempt_valid = 1u;
    o->repair_attempt_kind = 2u;
    o->repair_attempt_source = get;
    o->repair_attempt_put = put;
    o->repair_attempt_command = word;
    o->repair_attempt_target = get;
    o->repair_attempt_word = word;
    o->stats.generated_link_attempts++;
    const int sequential_previous =
        !o->packet_active && o->packet_count &&
        o->packet_next_get == get && o->packet_next_ret == ret &&
        o->packet_get != get;
    const u32 previous_get = sequential_previous
        ? o->packet_get : ~0u;
    const u32 previous_command = sequential_previous
        ? o->packet_command : 0u;
    const int resolved = o->resolve_hole(
        o->resolve_hole_user, get, put, word,
        previous_get, previous_command, &resume);
    if (resolved <= 0) {
        if (resolved < 0) {
            o->repair_attempt_valid = 0u;
            o->flow_wait_put_polls = 0u;
        }
        return 0;
    }
    if (resume == get || !frame_read(o, get, &current) || current != word ||
        !frame_read(o, resume, &resume_word) ||
        !frame_flow_target_ready(o, resume, ret, resume_word)) {
        o->repair_attempt_valid = 0u;
        o->flow_wait_put_polls = 0u;
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

static rsx_nr_frame_step_result frame_drain_method(rsx_nr_frame_owner* o)
{
    while (rsx_nr_ring_depth(o->ring)) {
        const unsigned long long ops_before =
            o->backend->stats.executed[RSX_NIR_OP_PRESENT];
        const rsx_nr_step_result result = rsx_nr_backend_step(o->backend);
        if (result == RSX_NR_STEP_BLOCKED_SEMAPHORE ||
            result == RSX_NR_STEP_BLOCKED_TOKEN) {
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
            return RSX_NR_FRAME_ADVANCED;
        }

        const u32 method = o->packet_non_incrementing
            ? o->packet_method : o->packet_method + o->packet_index * 4u;
        u32 argument = 0;
        const u32 argument_io = o->packet_get + 4u + o->packet_index * 4u;
        if (!frame_read(o, argument_io, &argument))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_UNMAPPED, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method, 0u,
                o->packet_index);
        if (!rsx_nir_adapter_method_supported(o->adapter, method, argument))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);
        if (!rsx_nr_ring_can_accept(
                o->ring, NR_FRAME_METHOD_OP_BOUND,
                NR_FRAME_METHOD_SIDE_BOUND))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);

        rsx_nr_ring_clear_reject(o->ring);
        o->method_errors_before = o->backend->stats.exec_errors;
        o->packet_argument = argument;
        rsx_nir_adapter_method(o->adapter, method, argument);
        o->stats.methods++;
        if (rsx_nr_ring_reject_sticky(o->ring))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_put, o->packet_ret, o->packet_command, method,
                argument,
                o->packet_index);
        o->method_inflight = rsx_nr_ring_depth(o->ring) != 0u;
        if (!o->method_inflight) {
            o->packet_index++;
            continue;
        }
    }
}

rsx_nr_frame_step_result rsx_nr_frame_owner_step(
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
        if (o->island_edge && o->island_edge(
                o->island_edge_user, get, put, command,
                &island_resume)) {
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
             * publication wait.  Called lists remain strict failures. */
            if (call_return == ~0u && get < NR_FRAME_RING_SIZE)
                return frame_wait_for_flow_target(
                    o, get, put, call_return, command, get, command);
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, put, call_return,
                command, 0u, target, 0u);
        }
        if (!frame_flow_target_ready(
                o, target, call_return, target_word)) {
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
