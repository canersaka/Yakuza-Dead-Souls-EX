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
const yz_runtime_config g_yz_runtime_config = {0};

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

static void feed_stream(rxs_stream* memory, const rxs_stream* frame)
{
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
    for (u32 i = 0; i < frame->n_records; i++) {
        const u32 method = frame->records[i * 2u];
        const u32 argument = frame->records[i * 2u + 1u];
        if (method & 0x80000000u)
            apply_block_to(frame, memory->arena, argument);
        else
            rsx_live_draw_method(method, argument);
    }
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
    u32 repeats = argc >= 4 ? (u32)strtoul(argv[3], NULL, 10) : 1u;
    if (!repeats || repeats > 64u) repeats = 1u;
    for (u32 repeat = 0; repeat < repeats; repeat++)
        feed_stream(&stream, &stream);
    for (int argument = 4; argument < argc; argument++) {
        rxs_stream later = {0};
        if (rxs_load(argv[argument], &later, 0)) {
            rsx_live_draw_shutdown();
            DestroyWindow(window);
            rxs_free(&stream);
            return 1;
        }
        fprintf(stderr, "[live-replay-sequence] feed %s\n", argv[argument]);
        feed_stream(&stream, &later);
        rxs_free(&later);
    }
    rsx_live_draw_flush();
    const int world = dump_boundary(argv[2], 0x01800000u, "world");
    const int final = dump_boundary(argv[2], 0x00E40000u, "final");
    const int scanout = dump_boundary(argv[2], 0x003C0000u, "scanout");
    fprintf(stderr,
            "[live-replay-boundary] result world=%d final=%d scanout=%d "
            "frames=%u draws=%u\n",
            world, final, scanout, rsx_live_draw_get_frames(),
            rsx_live_draw_get_last_draws());
    rsx_live_draw_shutdown();
    DestroyWindow(window);
    rxs_free(&stream);
    return world == 0 ? 0 : 1;
}
