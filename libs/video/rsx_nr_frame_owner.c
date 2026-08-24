/* Strict native RSX frame owner. See rsx_nr_frame_owner.h. */

#include "rsx_nr_frame_owner.h"

#include <string.h>

#define NR_FRAME_RING_SIZE 0x800000u
#define NR_FRAME_RING_MASK (NR_FRAME_RING_SIZE - 1u)
#define NR_FRAME_METHOD_OP_BOUND 512u
#define NR_FRAME_METHOD_SIDE_BOUND 16384u
#define NR_FRAME_CONTROL_BOUND 4096u

static rsx_nr_frame_step_result frame_fail(
    rsx_nr_frame_owner* o, u32 kind, u32 get, u32 ret, u32 command,
    u32 method, u32 argument, u32 index)
{
    if (!o->fatal) {
        o->failure.kind = kind;
        o->failure.get = get;
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
    rsx_nr_frame_owner* o, u32 get, u32 ret, u32 command)
{
    o->stats.control_words++;
    if (++o->control_streak > NR_FRAME_CONTROL_BOUND)
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, ret, command,
            0u, 0u, o->control_streak);
    return RSX_NR_FRAME_ADVANCED;
}

void rsx_nr_frame_owner_init(rsx_nr_frame_owner* o,
                             rsx_nir_adapter* adapter,
                             rsx_nr_backend* backend,
                             rsx_nr_ring* ring,
                             rsx_nr_frame_read32_fn read32,
                             void* read_user)
{
    memset(o, 0, sizeof(*o));
    o->adapter = adapter;
    o->backend = backend;
    o->ring = ring;
    o->read32 = read32;
    o->read_user = read_user;
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
                o->packet_ret, o->packet_command,
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
            o, RSX_NR_FRAME_FAILURE_CURSOR_CHANGED, get, call_return,
            o->packet_command, o->packet_method, 0u, o->packet_index);

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
                o->packet_ret, o->packet_command, method, 0u,
                o->packet_index);
        if (!rsx_nir_adapter_method_supported(o->adapter, method, argument))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_UNSUPPORTED_METHOD, o->packet_get,
                o->packet_ret, o->packet_command, method, argument,
                o->packet_index);
        if (!rsx_nr_ring_can_accept(
                o->ring, NR_FRAME_METHOD_OP_BOUND,
                NR_FRAME_METHOD_SIDE_BOUND))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_ret, o->packet_command, method, argument,
                o->packet_index);

        rsx_nr_ring_clear_reject(o->ring);
        o->method_errors_before = o->backend->stats.exec_errors;
        o->packet_argument = argument;
        rsx_nir_adapter_method(o->adapter, method, argument);
        o->stats.methods++;
        if (rsx_nr_ring_reject_sticky(o->ring))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_RING_CAPACITY, o->packet_get,
                o->packet_ret, o->packet_command, method, argument,
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
            o, RSX_NR_FRAME_FAILURE_UNMAPPED, get, call_return,
            0u, 0u, 0u, 0u);

    if (command == 0u) {
        *next_get = frame_linear_next(get, 4u, call_return);
        return frame_control_advance(o, get, call_return, command);
    }
    if ((command & 0xE0000003u) == 0x20000000u ||
        (command & 3u) == 1u) {
        const u32 target = (command & 3u) == 1u
            ? (command & 0xFFFFFFFCu) : (command & 0x1FFFFFFCu);
        u32 target_word = 0;
        if (target == get) {
            o->stats.waits_stopper++;
            return RSX_NR_FRAME_WAIT_STOPPER;
        }
        if (!frame_read(o, target, &target_word))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, call_return,
                command, 0u, target, 0u);
        *next_get = target;
        return frame_control_advance(o, get, call_return, command);
    }
    if ((command & 3u) == 2u) {
        const u32 target = command & 0x1FFFFFFCu;
        u32 target_word = 0;
        if (call_return != ~0u || !frame_read(o, target, &target_word))
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, call_return,
                command, 0u, target, 0u);
        *next_return = get < NR_FRAME_RING_SIZE
            ? ((get + 4u) & NR_FRAME_RING_MASK) : get + 4u;
        *next_get = target;
        return frame_control_advance(o, get, call_return, command);
    }
    if ((command & 0xFFFF0003u) == 0x00020000u) {
        if (call_return == ~0u)
            return frame_fail(
                o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, call_return,
                command, 0u, 0u, 0u);
        *next_get = call_return;
        *next_return = ~0u;
        return frame_control_advance(o, get, call_return, command);
    }
    if (command & 0xA0030003u)
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, call_return,
            command, 0u, 0u, 0u);

    const u32 count = (command >> 18) & 0x7FFu;
    if (!count)
        return frame_fail(
            o, RSX_NR_FRAME_FAILURE_BAD_FLOW, get, call_return,
            command, 0u, 0u, 0u);
    const u32 bytes = 4u + count * 4u;
    if (rsx_nr_fifo_section_range_status(
            get, bytes, put, call_return, NR_FRAME_RING_SIZE) !=
        RSX_NR_FIFO_RANGE_READY) {
        o->stats.waits_partial++;
        return RSX_NR_FRAME_WAIT_PARTIAL;
    }

    o->packet_active = 1;
    o->control_streak = 0;
    o->packet_get = get;
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
