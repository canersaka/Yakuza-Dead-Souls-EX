/*
 * Exact-PC orphanage renderer checkpoint.
 *
 * This is diagnostic-only and inert unless YZ_STAGE_RENDER_CHECKPOINT is set.
 * The generated PPU chunk calls phase 0 at guest PC 0x00113584, immediately
 * before the B0/B2/FC/100 gate sequence, and phase 1 at 0x0011486C.  We copy
 * only the r24 selection chain and reachable stage-render object graph.  The
 * resulting compact backing store is consumed by tools/stage_checkpoint.py.
 */

#include "ppu_recomp.h"
#include "rsx_live_draw.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" uint8_t* vm_base;
extern "C" unsigned long long __stdcall GetTickCount64(void);

namespace
{
constexpr uint32_t checkpoint_pc = 0x00113584u;
constexpr uint32_t checkpoint_end_pc = 0x0011486Cu;
constexpr uint32_t default_target_gmd = 0x429D8A00u;
constexpr uint32_t command_arena_slot = 0x0163B980u;
constexpr uint32_t primary_transform_bank = 0x015BCF40u;
constexpr uint32_t secondary_transform_bank = 0x015BD080u;
constexpr uint32_t shared_stage_transform = 0x01622590u;
constexpr uint32_t transform_bank_bytes = 5u * 0x40u;
constexpr uint32_t shared_transform_bytes = 0x100u;
constexpr uint32_t max_mesh_count = 4096u;
constexpr uint32_t record_stride = 12u;

struct region
{
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
};

struct command_state
{
    uint32_t pointer = 0;
    uint32_t head = 0;
    uint32_t end = 0;
    uint32_t count = 0;
    bool readable = false;
};

struct checkpoint_snapshot
{
    bool armed = false;
    uint32_t sequence = 0;
    uint32_t object = 0;
    uint32_t pass_index = 0;
    uint32_t object_index = 0;
    unsigned long long captured_tick = 0;
    uint64_t gpr[32] = {};
    uint32_t cr = 0;
    uint64_t lr = 0;
    uint64_t ctr = 0;
    uint32_t xer = 0;
    uint64_t thread_id = 0;
    command_state before = {};
    command_state after = {};
    std::vector<region> regions;
};

checkpoint_snapshot g_checkpoint;
uint32_t g_completed_checkpoints = 0;

uint32_t checkpoint_limit()
{
    static const uint32_t value = [] {
        const char* configured = std::getenv("YZ_STAGE_RENDER_CHECKPOINT_COUNT");
        if (!configured || !configured[0])
            return 1u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(configured, &end, 0);
        if (!end || *end != '\0' || parsed == 0)
            return 1u;
        return static_cast<uint32_t>(std::min(parsed, 256ul));
    }();
    return value;
}

std::string checkpoint_stem(uint32_t sequence)
{
    const char* configured = std::getenv("YZ_STAGE_RENDER_CHECKPOINT");
    std::string path =
        configured && configured[0] && std::strcmp(configured, "1") != 0
            ? configured
            : "scratch\\a010_recomp_stage_checkpoint_regions.json";

    static constexpr const char regions_suffix[] = "_regions.json";
    if (path.size() >= sizeof(regions_suffix) - 1u &&
        path.compare(
            path.size() - (sizeof(regions_suffix) - 1u),
            sizeof(regions_suffix) - 1u,
            regions_suffix) == 0) {
        path.resize(path.size() - (sizeof(regions_suffix) - 1u));
    } else if (path.size() >= 5u &&
               path.compare(path.size() - 5u, 5u, ".json") == 0) {
        path.resize(path.size() - 5u);
    }

    if (checkpoint_limit() > 1u) {
        char suffix[16] = {};
        std::snprintf(suffix, sizeof(suffix), "_%03u", sequence);
        path += suffix;
    }
    return path;
}

uint32_t target_gmd()
{
    static const uint32_t value = [] {
        const char* configured = std::getenv("YZ_STAGE_RENDER_GMD");
        if (!configured || !configured[0])
            return default_target_gmd;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(configured, &end, 0);
        return end && *end == '\0' && parsed <= 0xFFFFFFFFul
            ? static_cast<uint32_t>(parsed)
            : default_target_gmd;
    }();
    return value;
}

bool guest_range(uint32_t address, uint32_t size)
{
    if (!address || !size)
        return false;
    const uint64_t end = static_cast<uint64_t>(address) + size;
    if (end > 0xE0000000ull)
        return false;
    if (address < 0x00010000u)
        return false;

    const uint8_t* cursor = vm_base + address;
    const uint8_t* const finish = vm_base + end;
    while (cursor < finish) {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(cursor, &info, sizeof(info)) ||
            info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const uint8_t* const region_end =
            static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
        if (region_end <= cursor)
            return false;
        cursor = std::min(region_end, finish);
    }
    return true;
}

uint32_t bounded_mesh_count(uint32_t object)
{
    const uint32_t begin = vm_read32(object + 0x10u);
    const uint32_t end = vm_read32(object + 0x0Cu);
    if (end < begin)
        return 0;
    const uint32_t count = (end - begin) / 2u;
    return count <= max_mesh_count ? count : 0u;
}

void add_region(checkpoint_snapshot& snapshot, uint32_t address, uint32_t size)
{
    if (!guest_range(address, size))
        return;
    for (const auto& existing : snapshot.regions) {
        if (existing.address == address && existing.bytes.size() == size)
            return;
    }
    region item;
    item.address = address;
    item.bytes.resize(size);
    std::memcpy(item.bytes.data(), vm_base + address, size);
    snapshot.regions.emplace_back(std::move(item));
}

command_state read_command_state()
{
    command_state state;
    state.pointer = vm_read32(command_arena_slot);
    if (guest_range(state.pointer, 0x20u)) {
        state.readable = true;
        state.head = vm_read32(state.pointer + 0x00u);
        state.end = vm_read32(state.pointer + 0x04u);
        state.count = vm_read32(state.pointer + 0x1Cu);
    }
    return state;
}

void add_record_array(
    checkpoint_snapshot& snapshot,
    uint32_t pointer,
    uint32_t mesh_count)
{
    if (mesh_count)
        add_region(snapshot, pointer, mesh_count * record_stride);
}

void add_pointer_table(
    checkpoint_snapshot& snapshot,
    uint32_t pointer,
    uint32_t pass_count,
    uint32_t mesh_count)
{
    if (!pass_count || pass_count > 64u || !guest_range(pointer, pass_count * 4u))
        return;
    add_region(snapshot, pointer, pass_count * 4u);
    for (uint32_t index = 0; index < pass_count; ++index) {
        const uint32_t records = vm_read32(pointer + index * 4u);
        add_record_array(snapshot, records, mesh_count);
    }
}

void add_render_groups(
    checkpoint_snapshot& snapshot,
    uint32_t gmd)
{
    add_region(snapshot, gmd, 0x104u);
    const uint32_t table = vm_read32(gmd + 0xF4u);
    if (!guest_range(table, 0x100u))
        return;
    add_region(snapshot, table, 0x100u);
    for (uint32_t group = 0; group < 16u; ++group) {
        const uint32_t descriptor = table + group * 0x10u;
        const uint32_t count = vm_read32(descriptor + 0x00u);
        const uint32_t indices = vm_read32(descriptor + 0x04u);
        if (count <= max_mesh_count && count)
            add_region(snapshot, indices, count * 2u);
    }
}

void add_global_state(checkpoint_snapshot& snapshot)
{
    static constexpr uint32_t globals[] = {
        0x01364960u,
        0x01367100u,
        0x0163B0ACu,
        0x014EE738u,
        0x014EE734u,
        0x014EE728u,
        command_arena_slot,
    };
    for (const uint32_t address : globals)
        add_region(snapshot, address, 4u);

    const uint32_t manager = vm_read32(0x01364960u);
    if (guest_range(manager, 0x49Cu)) {
        add_region(snapshot, manager + 0x400u, 4u);
        add_region(snapshot, manager + 0x498u, 4u);
    }
    const uint32_t render_state = vm_read32(0x01367100u);
    if (guest_range(render_state, 0x518u))
        add_region(snapshot, render_state + 0x514u, 4u);
    const uint32_t arena = vm_read32(command_arena_slot);
    if (guest_range(arena, 0x20u))
        add_region(snapshot, arena, 0x20u);

    /*
     * The final object writer copies shared_stage_transform verbatim into
     * object+0x30.  Preserve its two five-slot producers and the shared
     * publication block so repeated visits can distinguish a late publish
     * from a missing producer without another boot.
     */
    add_region(snapshot, primary_transform_bank, transform_bank_bytes);
    add_region(snapshot, secondary_transform_bank, transform_bank_bytes);
    add_region(snapshot, shared_stage_transform, shared_transform_bytes);
}

void collect_snapshot(ppu_context* ctx)
{
    checkpoint_snapshot fresh;
    fresh.armed = true;
    fresh.sequence = g_completed_checkpoints;
    fresh.object = static_cast<uint32_t>(ctx->gpr[24]);
    fresh.pass_index = static_cast<uint32_t>(vm_read64(
        static_cast<uint32_t>(ctx->gpr[1]) + 0xC0u));
    fresh.object_index = vm_read32(
        static_cast<uint32_t>(ctx->gpr[1]) + 0xA4u);
    fresh.captured_tick = GetTickCount64();
    std::memcpy(fresh.gpr, ctx->gpr, sizeof(fresh.gpr));
    fresh.cr = ctx->cr;
    fresh.lr = ctx->lr;
    fresh.ctr = ctx->ctr;
    fresh.xer = ctx->xer;
    fresh.thread_id = ctx->thread_id;
    fresh.before = read_command_state();

    const uint32_t object = fresh.object;
    const uint32_t stack = static_cast<uint32_t>(ctx->gpr[1]);
    const uint32_t root = static_cast<uint32_t>(vm_read64(stack + 0xE0u));
    const uint32_t object_offset =
        static_cast<uint32_t>(vm_read64(stack + 0xB8u));
    const uint32_t list_base = vm_read32(root + 0x108u);
    const uint32_t wrapper_slot = list_base + object_offset;
    const uint32_t wrapper = vm_read32(wrapper_slot);

    add_region(fresh, stack + 0x80u, 0x78u);
    add_region(fresh, root + 0x108u, 4u);
    add_region(fresh, wrapper_slot, 4u);
    add_region(fresh, wrapper + 0x12Cu, 4u);
    add_region(fresh, object, 0x134u);

    const uint32_t mesh_count = bounded_mesh_count(object);
    const uint32_t mesh_flags = vm_read32(object + 0x10u);
    if (mesh_count)
        add_region(fresh, mesh_flags, mesh_count * 2u);
    add_record_array(fresh, vm_read32(object + 0xF4u), mesh_count);
    add_record_array(fresh, vm_read32(object + 0xF8u), mesh_count);

    const uint32_t pass_count = vm_read8(object + 0xB2u);
    add_pointer_table(
        fresh, vm_read32(object + 0xFCu), pass_count, mesh_count);
    add_pointer_table(
        fresh, vm_read32(object + 0x100u), pass_count, mesh_count);
    add_render_groups(fresh, vm_read32(object + 0x04u));
    add_global_state(fresh);
    g_checkpoint = std::move(fresh);
}

void print_hex(FILE* file, const std::vector<uint8_t>& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    char pair[2];
    for (const uint8_t value : bytes) {
        pair[0] = digits[value >> 4];
        pair[1] = digits[value & 0x0Fu];
        fwrite(pair, 1, 2, file);
    }
}

void write_regions(const checkpoint_snapshot& snapshot)
{
    const std::string path = checkpoint_stem(snapshot.sequence) + "_regions.json";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr,
            "[stage-checkpoint] could not open compact backing store %s\n",
            path.c_str());
        return;
    }
    fputs("{\"memory_regions\":[", file);
    for (size_t index = 0; index < snapshot.regions.size(); ++index) {
        const auto& item = snapshot.regions[index];
        std::fprintf(file,
            "%s{\"address\":\"0x%08x\",\"bytes\":\"",
            index ? "," : "", item.address);
        print_hex(file, item.bytes);
        fputs("\"}", file);
    }
    fputs("]}\n", file);
    fclose(file);
}

void write_command(FILE* file, const char* name, const command_state& state)
{
    std::fprintf(file,
        "\"%s\":{\"pointer\":\"0x%08x\",\"head\":\"0x%08x\","
        "\"end\":\"0x%08x\",\"count\":%u,\"readable\":%s}",
        name, state.pointer, state.head, state.end, state.count,
        state.readable ? "true" : "false");
}

void write_metadata(const checkpoint_snapshot& snapshot)
{
    const std::string path = checkpoint_stem(snapshot.sequence) + "_meta.json";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr,
            "[stage-checkpoint] could not open metadata %s\n", path.c_str());
        return;
    }
    std::fprintf(file,
        "{\"schema\":1,\"engine\":\"recomp\",\"sequence\":%u,"
        "\"pc\":\"0x%08x\","
        "\"end_pc\":\"0x%08x\",\"thread_id\":\"0x%016llx\","
        "\"cr\":\"0x%08x\",\"lr\":\"0x%016llx\","
        "\"ctr\":\"0x%016llx\",\"xer\":\"0x%08x\","
        "\"registers\":{",
        snapshot.sequence, checkpoint_pc, checkpoint_end_pc,
        static_cast<unsigned long long>(snapshot.thread_id),
        snapshot.cr,
        static_cast<unsigned long long>(snapshot.lr),
        static_cast<unsigned long long>(snapshot.ctr),
        snapshot.xer);
    for (unsigned index = 0; index < 32u; ++index) {
        std::fprintf(file, "%s\"r%u\":\"0x%016llx\"",
            index ? "," : "", index,
            static_cast<unsigned long long>(snapshot.gpr[index]));
    }
    std::fprintf(file,
        "},\"frame\":{\"host_tick_ms\":%llu,\"live_frame\":%u,"
        "\"last_frame_draws\":%u,\"pass_index\":%u,\"object_index\":%u},",
        snapshot.captured_tick,
        rsx_live_draw_get_frames(),
        rsx_live_draw_get_last_draws(),
        snapshot.pass_index,
        snapshot.object_index);
    write_command(file, "command_arena_before", snapshot.before);
    fputc(',', file);
    write_command(file, "command_arena_after", snapshot.after);
    fputs(",\"status\":\"complete\"}\n", file);
    fclose(file);
}
} // namespace

extern "C" void yz_stage_render_checkpoint(void* context, uint32_t phase)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = std::getenv("YZ_STAGE_RENDER_CHECKPOINT") ? 1 : 0;
    if (!enabled || g_completed_checkpoints >= checkpoint_limit())
        return;

    /*
     * Phase 1 is reached through a generic vm_read64 observer.  It may see
     * unrelated stack+0xB8 reads during boot, so it must not inspect r24 until
     * the exact phase-0 renderer checkpoint has armed a valid object.
     */
    if (phase > 1u || (phase == 1u && !g_checkpoint.armed))
        return;

    auto* ctx = static_cast<ppu_context*>(context);
    const uint32_t object = static_cast<uint32_t>(ctx->gpr[24]);
    if (!guest_range(object, 0x134u) ||
        vm_read32(object + 0x04u) != target_gmd())
        return;

    if (phase == 0u) {
        const uint32_t stack = static_cast<uint32_t>(ctx->gpr[1]);
        const uint32_t pass_index =
            static_cast<uint32_t>(vm_read64(stack + 0xC0u));
        if (pass_index != 0u || g_checkpoint.armed)
            return;
        collect_snapshot(ctx);
        std::fprintf(stderr,
            "[stage-checkpoint] ARMED sequence=%u/%u pc=%08X r24=%08X "
            "gmd=%08X pass=%u "
            "b0=%04X b2=%u regions=%zu\n",
            g_checkpoint.sequence, checkpoint_limit(),
            checkpoint_pc, object, target_gmd(), pass_index,
            static_cast<unsigned>(vm_read16(object + 0xB0u)),
            static_cast<unsigned>(vm_read8(object + 0xB2u)),
            g_checkpoint.regions.size());
        std::fflush(stderr);
        return;
    }

    if (phase == 1u && g_checkpoint.armed &&
        object == g_checkpoint.object) {
        g_checkpoint.after = read_command_state();
        write_regions(g_checkpoint);
        write_metadata(g_checkpoint);
        std::fprintf(stderr,
            "[stage-checkpoint] COMPLETE sequence=%u/%u pc=%08X->%08X r24=%08X "
            "cmd=%08X/%u->%08X/%u\n",
            g_checkpoint.sequence, checkpoint_limit(),
            checkpoint_pc, checkpoint_end_pc, object,
            g_checkpoint.before.head, g_checkpoint.before.count,
            g_checkpoint.after.head, g_checkpoint.after.count);
        std::fflush(stderr);
        ++g_completed_checkpoints;
        g_checkpoint = {};
    }
}
