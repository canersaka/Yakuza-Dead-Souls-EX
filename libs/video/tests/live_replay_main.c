/*
 * Feed an exported RXS frame through the production rsx_live_draw backend.
 * This is intentionally separate from replay_main.c: comparing their output
 * at the same guest surface catches live-only state/lifetime bugs while the
 * captured guest memory and method stream remain identical.
 */

#ifndef _WIN32
#error The live renderer differential harness is Windows-only.
#endif

#define _CRT_SECURE_NO_WARNINGS

#include "../rsx_live_draw.h"
#include "../rsx_nir_adapter.h"
#include "../rsx_nr_backend.h"
#include "../rsx_nr_backend_d3d12.h"
#include "../rsx_nr_ring.h"
#include "ps3emu/yz_runtime_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct {
    u32 location, offset, size, data_off;
} rxs_block;

typedef struct {
    u32 w, h, pitch, offset;
} rxs_display_buf;

typedef struct {
    u32 n_blocks, n_records, reg_words, vp_words, disp_w, disp_h;
    u32 const_words;
    u32 disp_count;
    rxs_display_buf disp[8];
    u32* regs;
    u32* vp;
    u32* constants;
    rxs_block* blocks;
    u8* data;
    u32* records;
    size_t data_size;
    u8* arena[2];
} rxs_stream;

#define ARENA_SIZE (256u * 1024u * 1024u)

volatile LONG g_yz_a010_reference_camera_active = 0;
volatile LONG g_yz_a010_root_active = 0;
volatile LONG g_yz_a010_release_scene_active = 0;
volatile LONG g_yz_a010_stage_generation = 0;
volatile LONG g_yz_auto_new_game_complete = 0;
volatile unsigned long long g_yz_auto_start_tick = 0;
u32 g_yz_codec_taskset = 0;
u32 g_yz_parked_pub_ea = 0;
u8* vm_base = NULL;
void (*g_yz_usleep_pump)(void) = NULL;
const yz_runtime_config g_yz_runtime_config = {0};

/* The production runtime library contains optional boot/SPU diagnostics whose
 * owners live in the game executable.  Replay never exercises those paths,
 * but keeping inert definitions here lets this differential harness link
 * against the exact current renderer instead of a stale standalone binary. */
u32 yz_thread_current_id(void) { return 0; }
u32 yz_guest_addr_from_host(const void* rip) { (void)rip; return 0; }
void yz_movement_frontier_snapshot(const char* reason) { (void)reason; }
int yz_ea_trap_range(u32 ea, unsigned size, u32* lo, u32* hi)
{
    (void)ea; (void)size; (void)lo; (void)hi;
    return 0;
}
void yz_a010_reltrace_spu(
    u32 spu_id, u32 image_id, u32 pc, u32 ea,
    const u8* payload, u32 size, const u32* context)
{
    (void)spu_id; (void)image_id; (void)pc; (void)ea;
    (void)payload; (void)size; (void)context;
}
void yz_a010_reltrace_spu_commit(
    u32 spu_id, u32 image_id, u32 pc, u32 ea,
    const u8* payload, u32 size)
{
    (void)spu_id; (void)image_id; (void)pc; (void)ea;
    (void)payload; (void)size;
}

void spu_perf_window_begin(u32 frame) { (void)frame; }
void spu_perf_window_dump(u32 frame) { (void)frame; }
void spu_perf_frame_sample_record(u32 frame) { (void)frame; }
void spu_perf_frame_sample_dump(u32 start, u32 end)
{ (void)start; (void)end; }
void yz_ppu_perf_window_begin(u32 frame) { (void)frame; }
void yz_ppu_perf_window_dump(u32 frame) { (void)frame; }

static void rxs_free(rxs_stream* stream)
{
    if (!stream) return;
    free(stream->records);
    free(stream->data);
    free(stream->blocks);
    free(stream->constants);
    free(stream->vp);
    free(stream->regs);
    for (u32 i = 0; i < 2; i++)
        if (stream->arena[i]) VirtualFree(stream->arena[i], 0, MEM_RELEASE);
    memset(stream, 0, sizeof(*stream));
}

static int read_exact(FILE* file, void* destination, size_t size)
{
    return size == 0 || fread(destination, 1, size, file) == size;
}

static int rxs_load(const char* path, rxs_stream* stream, int allocate_arenas)
{
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    u32 header[8];
    if (!read_exact(file, header, sizeof(header)) ||
        memcmp(header, "RXS1", 4) != 0 ||
        (header[1] != 2 && header[1] != 3)) {
        fprintf(stderr, "%s is not an RXS1 v2/v3 stream\n", path);
        fclose(file);
        return -1;
    }
    stream->n_blocks = header[2];
    stream->n_records = header[3];
    stream->reg_words = header[4];
    stream->vp_words = header[5];
    stream->disp_w = header[6];
    stream->disp_h = header[7];
    if (!read_exact(file, &stream->disp_count, sizeof(stream->disp_count)) ||
        !read_exact(file, stream->disp, sizeof(stream->disp)) ||
        (header[1] >= 3 &&
         !read_exact(file, &stream->const_words,
                     sizeof(stream->const_words)))) {
        fprintf(stderr, "%s has a truncated display/header table\n", path);
        fclose(file);
        return -1;
    }

    stream->regs = malloc((size_t)stream->reg_words * sizeof(u32));
    stream->vp = malloc((size_t)stream->vp_words * sizeof(u32));
    stream->constants = malloc(stream->const_words
        ? (size_t)stream->const_words * sizeof(u32) : 1u);
    stream->blocks = malloc((size_t)stream->n_blocks * sizeof(rxs_block));
    if (!stream->regs || !stream->vp || !stream->constants ||
        !stream->blocks ||
        !read_exact(file, stream->regs,
                    (size_t)stream->reg_words * sizeof(u32)) ||
        !read_exact(file, stream->vp,
                    (size_t)stream->vp_words * sizeof(u32)) ||
        !read_exact(file, stream->constants,
                    (size_t)stream->const_words * sizeof(u32)) ||
        !read_exact(file, stream->blocks,
                    (size_t)stream->n_blocks * sizeof(rxs_block))) {
        fprintf(stderr, "%s has truncated state sections\n", path);
        fclose(file);
        rxs_free(stream);
        return -1;
    }
    for (u32 i = 0; i < stream->n_blocks; i++) {
        const size_t end = (size_t)stream->blocks[i].data_off +
                           stream->blocks[i].size;
        if (end < stream->blocks[i].data_off) {
            fclose(file);
            rxs_free(stream);
            return -1;
        }
        if (end > stream->data_size) stream->data_size = end;
    }
    stream->data = malloc(stream->data_size ? stream->data_size : 1u);
    stream->records = malloc((size_t)stream->n_records * 2u * sizeof(u32));
    if (!stream->data || !stream->records ||
        !read_exact(file, stream->data, stream->data_size) ||
        !read_exact(file, stream->records,
                    (size_t)stream->n_records * 2u * sizeof(u32))) {
        fprintf(stderr, "%s has truncated data/records\n", path);
        fclose(file);
        rxs_free(stream);
        return -1;
    }
    fclose(file);
    if (allocate_arenas) {
        for (u32 i = 0; i < 2; i++) {
            stream->arena[i] = VirtualAlloc(
                NULL, ARENA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!stream->arena[i]) {
                fprintf(stderr, "guest arena allocation failed\n");
                rxs_free(stream);
                return -1;
            }
        }
    }
    return 0;
}

static const u8* live_guest_ptr(
    void* user, u32 location, u32 offset, u32 min_bytes)
{
    rxs_stream* stream = user;
    if (!stream || location > 1 || offset > ARENA_SIZE ||
        min_bytes > ARENA_SIZE - offset)
        return NULL;
    return stream->arena[location] + offset;
}

static void apply_block_to(
    const rxs_stream* stream, u8* const arena[2], u32 index)
{
    if (!stream || !arena || index >= stream->n_blocks) return;
    const rxs_block* block = &stream->blocks[index];
    if (block->location > 1 || block->offset > ARENA_SIZE ||
        block->size > ARENA_SIZE - block->offset ||
        block->data_off > stream->data_size ||
        block->size > stream->data_size - block->data_off)
        return;
    memcpy(arena[block->location] + block->offset,
           stream->data + block->data_off, block->size);
}

typedef struct mixed_renderer {
    rsx_nr_d3d12* d3d12;
    rsx_nr_ring ring;
    rsx_nr_tokens tokens;
    rsx_nr_backend backend;
    rsx_nir_adapter* adapter;
    u32 target_draw;
    u32 last_draw;
    u32 completed_draws;
    int executed;
} mixed_renderer;

static u8* mixed_guest_wptr(
    void* user, u32 location, u32 offset, u32 min_bytes)
{
    return (u8*)live_guest_ptr(user, location, offset, min_bytes);
}

static int mixed_borrow_color(
    void* user, u32 location, u32 offset, u32 width, u32 height, int create,
    void** resource, u32* format, u32* resource_width, u32* resource_height)
{
    (void)user;
    return rsx_live_draw_borrow_color(
        location, offset, width, height, create, resource, format,
        resource_width, resource_height);
}

static int mixed_borrow_depth(
    void* user, u32 location, u32 offset, u32 depth_format,
    u32 width, u32 height, int create, void** resource,
    u32* resource_format, u32* dsv_format, u32* srv_format,
    void** sample_resource, u32* sample_srv_format,
    int* publication_required)
{
    (void)user;
    return rsx_live_draw_borrow_depth(
        location, offset, depth_format, width, height, create, resource,
        resource_format, dsv_format, srv_format, sample_resource,
        sample_srv_format, publication_required);
}

static int mixed_resolve_depth(
    void* user, u32 location, u32 offset, u32 width, u32 height)
{
    (void)user;
    return rsx_live_draw_resolve_depth_sample(
        location, offset, width, height);
}

static int mixed_timeline_acquire(
    void* user, void** list, u64* generation, u64* recording,
    u64* completed)
{
    (void)user;
    return rsx_live_draw_timeline_acquire(
        list, generation, recording, completed);
}

static void mixed_timeline_release(void* user)
{
    (void)user;
    rsx_live_draw_timeline_release();
}

static int mixed_timeline_flush(void* user)
{
    (void)user;
    return rsx_live_draw_timeline_flush();
}

static int mixed_init(mixed_renderer* mixed, rxs_stream* memory,
                      const rxs_stream* frame, u32 target_draw,
                      u32 draw_count)
{
    memset(mixed, 0, sizeof(*mixed));
    mixed->target_draw = target_draw;
    mixed->last_draw = target_draw + draw_count - 1u;
    mixed->d3d12 = rsx_nr_d3d12_create(
        rsx_live_draw_get_d3d12_device(), ARENA_SIZE, ARENA_SIZE,
        live_guest_ptr, mixed_guest_wptr, memory);
    if (!mixed->d3d12 ||
        rsx_nr_d3d12_set_coherent_section_mode(mixed->d3d12, 1) != 0)
        return -1;
    rsx_nr_d3d12_set_resource_broker(
        mixed->d3d12, mixed_borrow_color, mixed_borrow_depth,
        mixed_resolve_depth, NULL);
    if (rsx_nr_d3d12_set_shared_timeline(
            mixed->d3d12, mixed_timeline_acquire,
            mixed_timeline_release, mixed_timeline_flush, NULL) != 0)
        return -1;
    if (rsx_nr_ring_init(&mixed->ring, 4096u, 1u << 19))
        return -1;
    rsx_nr_tokens_init(&mixed->tokens);
    rsx_nr_exec_ops ops = {0};
    rsx_nr_d3d12_get_exec_ops(mixed->d3d12, &ops);
    rsx_nr_backend_init(
        &mixed->backend, &mixed->ring, &mixed->tokens, &ops);
    mixed->adapter = malloc(sizeof(*mixed->adapter));
    if (!mixed->adapter)
        return -1;
    const rsx_nir_sink sink = rsx_nr_ring_sink(&mixed->ring);
    rsx_nir_adapter_init_sink(mixed->adapter, &sink);
    rsx_nir_adapter_seed(
        mixed->adapter, frame->regs, frame->reg_words,
        frame->vp, frame->vp_words, frame->constants, frame->const_words);
    mixed->adapter->shadow_mode = 1;
    return 0;
}

static void mixed_destroy(mixed_renderer* mixed)
{
    if (!mixed) return;
    free(mixed->adapter);
    rsx_nr_ring_destroy(&mixed->ring);
    rsx_nr_d3d12_destroy(mixed->d3d12);
    memset(mixed, 0, sizeof(*mixed));
}

static int mixed_method(mixed_renderer* mixed, u32 method, u32 argument)
{
    const int terminal = method == 0x1808u && argument == 0u;
    const u32 next_draw = mixed->completed_draws + 1u;
    if (!terminal || next_draw < mixed->target_draw ||
        next_draw > mixed->last_draw) {
        rsx_nir_adapter_method(mixed->adapter, method, argument);
        rsx_live_draw_method(method, argument);
        if (terminal)
            mixed->completed_draws++;
        return 0;
    }

    mixed->completed_draws = next_draw;
    rsx_live_draw_mirror_state_method(method, argument);
    if (!rsx_nir_adapter_shadow_action(
            mixed->adapter, method, argument))
        return -1;
    while (rsx_nr_ring_depth(&mixed->ring)) {
        const rsx_nr_step_result step = rsx_nr_backend_step(&mixed->backend);
        if (step != RSX_NR_STEP_EXECUTED)
            return -1;
    }
    rsx_live_draw_native_draw_commit(&mixed->backend.st);
    mixed->executed++;
    return next_draw == mixed->last_draw ? 1 : 0;
}

static int feed_stream(rxs_stream* memory, const rxs_stream* frame,
                       mixed_renderer* mixed)
{
    if (mixed)
        mixed->completed_draws = 0;
    rsx_live_draw_seed_registers(frame->regs, frame->reg_words);
    rsx_live_draw_seed_transform_program(frame->vp, frame->vp_words);
    rsx_live_draw_seed_transform_constants(
        frame->constants, frame->const_words);
    for (u32 i = 0; i < frame->disp_count && i < 8; i++) {
        const rxs_display_buf* display = &frame->disp[i];
        rsx_live_draw_set_display_buffer(
            i, 0, display->offset, display->pitch,
            display->w, display->h);
    }
    u32 completed_draws = 0;
    u32 stop_after_draw = 0;
    const char* const stop = getenv("YZ_NR_CAPTURE_STOP_AFTER_DRAW");
    if (stop && stop[0])
        stop_after_draw = (u32)strtoul(stop, NULL, 0);
    for (u32 i = 0; i < frame->n_records; i++) {
        const u32 method = frame->records[i * 2u];
        const u32 argument = frame->records[i * 2u + 1u];
        if (method & 0x80000000u)
        {
            apply_block_to(frame, memory->arena, argument);
            if (mixed && argument < frame->n_blocks) {
                const rxs_block* const block = &frame->blocks[argument];
                rsx_nr_d3d12_note_guest_write(
                    mixed->d3d12, block->location, block->offset,
                    block->size);
            }
        }
        else {
            if (mixed) {
                const int result = mixed_method(mixed, method, argument);
                if (result < 0)
                    return -1;
                if (result > 0 && !getenv("YZ_NR_MIXED_CONTINUE"))
                    break;
            } else {
                rsx_live_draw_method(method, argument);
            }
            if (method == 0x1808u && argument == 0u &&
                ++completed_draws == stop_after_draw)
                break;
        }
    }
    return 0;
}

static LRESULT CALLBACK live_replay_window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND make_hidden_window(HINSTANCE instance)
{
    const char* class_name = "ps3recomp-live-replay";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = live_replay_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    return CreateWindowExA(
        0, class_name, class_name, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 64, 64,
        NULL, NULL, instance, NULL);
}

static int dump_boundary(const char* outdir, u32 offset, const char* label)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\live_%s_%08X.ppm",
             outdir, label, offset);
    return rsx_live_draw_debug_dump_surface(0, offset, path);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <stream.rxs> <output-dir> [repeat-count] "
                "[later-frame.rxs ...]\n",
                argv[0]);
        return 2;
    }
    rxs_stream stream = {0};
    if (rxs_load(argv[1], &stream, 1)) return 1;
    vm_base = stream.arena[0];
    CreateDirectoryA(argv[2], NULL);
    _putenv_s("YZ_RSX_DRAW", "1");
    if (!getenv("YZ_RSX_VERTEX_MODE"))
        _putenv_s("YZ_RSX_VERTEX_MODE", "C");
    if (!getenv("YZ_RSX_FP_CONSTANT_MODE"))
        _putenv_s("YZ_RSX_FP_CONSTANT_MODE", "BUFFERED");

    HINSTANCE instance = GetModuleHandleA(NULL);
    HWND window = make_hidden_window(instance);
    if (!window) {
        fprintf(stderr, "hidden replay window creation failed\n");
        rxs_free(&stream);
        return 1;
    }
    const u32 width = stream.disp_w ? stream.disp_w : 1280u;
    const u32 height = stream.disp_h ? stream.disp_h : 720u;
    if (rsx_live_draw_init(
            window, width, height, live_guest_ptr, &stream) != 0) {
        fprintf(stderr, "live renderer initialization failed\n");
        DestroyWindow(window);
        rxs_free(&stream);
        return 1;
    }
    mixed_renderer mixed = {0};
    mixed_renderer* mixed_ptr = NULL;
    const char* const mixed_draw_text = getenv("YZ_NR_MIXED_DRAW");
    if (mixed_draw_text && mixed_draw_text[0]) {
        const u32 mixed_draw = (u32)strtoul(mixed_draw_text, NULL, 0);
        const char* const count_text = getenv("YZ_NR_MIXED_DRAW_COUNT");
        u32 mixed_draw_count = count_text && count_text[0]
            ? (u32)strtoul(count_text, NULL, 0) : 1u;
        if (!mixed_draw || !mixed_draw_count ||
            mixed_draw_count - 1u > UINT32_MAX - mixed_draw ||
            mixed_init(&mixed, &stream, &stream, mixed_draw,
                       mixed_draw_count)) {
            fprintf(stderr, "mixed renderer initialization failed\n");
            rsx_live_draw_shutdown();
            DestroyWindow(window);
            rxs_free(&stream);
            return 1;
        }
        mixed_ptr = &mixed;
    }
    u32 repeats = argc >= 4 ? (u32)strtoul(argv[3], NULL, 10) : 1u;
    if (!repeats || repeats > 64u) repeats = 1u;
    for (u32 repeat = 0; repeat < repeats; repeat++) {
        if (feed_stream(&stream, &stream, mixed_ptr)) {
            fprintf(stderr, "mixed renderer execution failed\n");
            mixed_destroy(&mixed);
            rsx_live_draw_shutdown();
            DestroyWindow(window);
            rxs_free(&stream);
            return 1;
        }
    }
    for (int argument = 4; argument < argc; argument++) {
        rxs_stream later = {0};
        if (rxs_load(argv[argument], &later, 0)) {
            rsx_live_draw_shutdown();
            DestroyWindow(window);
            rxs_free(&stream);
            return 1;
        }
        fprintf(stderr, "[live-replay-sequence] feed %s\n", argv[argument]);
        feed_stream(&stream, &later, mixed_ptr);
        rxs_free(&later);
    }
    if (mixed_ptr)
        fprintf(stderr,
                "[mixed-replay] target=%u..%u completed=%u executed=%d\n",
                mixed.target_draw, mixed.last_draw,
                mixed.completed_draws, mixed.executed);
    rsx_live_draw_flush();
    const int world = dump_boundary(argv[2], 0x01800000u, "world");
    const int final = dump_boundary(argv[2], 0x00E40000u, "final");
    const int scanout = dump_boundary(argv[2], 0x003C0000u, "scanout");
    fprintf(stderr,
            "[live-replay-boundary] result world=%d final=%d scanout=%d "
            "frames=%u draws=%u\n",
            world, final, scanout, rsx_live_draw_get_frames(),
            rsx_live_draw_get_last_draws());
    mixed_destroy(&mixed);
    rsx_live_draw_shutdown();
    DestroyWindow(window);
    rxs_free(&stream);
    vm_base = NULL;
    return world == 0 ? 0 : 1;
}
