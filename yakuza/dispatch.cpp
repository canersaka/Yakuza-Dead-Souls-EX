/*
 * Indirect-call dispatcher for the recompiled Yakuza: Dead Souls EBOOT.
 *
 * The generated code (ppu_recomp.c) calls ps3_indirect_call(ctx) for
 * bctr/bctrl and expects:
 *   - the guest target address in ctx->ctr,
 *   - the dispatcher to resolve it to a host function (with OPD fallback:
 *     if ctr points at an OPD descriptor, word0 = code addr, word1 = TOC),
 *   - cross-fragment tail branches to be flattened via the thread-local
 *     g_trampoline_fn + the DRAIN_TRAMPOLINE loops the lifter emits.
 *
 * We resolve the target and set g_trampoline_fn instead of calling directly,
 * so long bctr tail-chains don't grow the host stack; the drain loop at the
 * call site (or in main) executes it iteratively.
 */

#include "ppu_recomp.h"
#include "yakuza_runner.h"
#include "rsx_live_draw.h"
#include "ps3emu/yz_runtime_config.h"
#include "ps3emu/yz_frontier_trace.h"

#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

#if defined(YZ_PERF_CLEAN)
#define YZ_DIAG_ENABLED(name) false
#define YZ_DIAG_VALUE(name) nullptr
#else
#define YZ_DIAG_ENABLED(name) (std::getenv(name) != nullptr)
#define YZ_DIAG_VALUE(name) std::getenv(name)
#endif

/* ntdll/kernel32 stack-walk (no windows.h here); links via kernel32.lib. */
extern "C" unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long FramesToSkip, unsigned long FramesToCapture,
    void** BackTrace, unsigned long* BackTraceHash);
extern "C" unsigned long long __stdcall GetTickCount64(void);
extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);
extern "C" __declspec(dllimport) void __stdcall WakeByAddressAll(void*);
struct yz_filetime {
    unsigned long low;
    unsigned long high;
};
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall GetThreadTimes(
    void* thread, yz_filetime* creation, yz_filetime* exit,
    yz_filetime* kernel, yz_filetime* user);

static uint64_t yz_filetime_ticks(const yz_filetime& value)
{
    return ((uint64_t)value.high << 32) | value.low;
}

/* TLS trampoline slot -- the generated code declares this extern and both
 * sets it (direct tail branches) and drains it. */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;
extern "C" volatile long g_yz_cri_yield_phase;
extern "C" volatile long g_yz_movie_stop_pending_serial;
extern "C" volatile long g_yz_movie_stop_applied_serial;
/* Latest GCM user-command cause whose game callback actually began running.
 * The FIFO consumer uses this acknowledgement to distinguish a queued event
 * from one that was lost before callback dispatch. */
extern "C" volatile long g_yz_ucmd_handler_arg;
extern "C" volatile long g_yz_ucmd_handler_completed;
extern "C" volatile long g_yz_ucmd_handler_epoch;
extern "C" volatile long g_yz_ucmd_handler_completed_epoch;
extern "C" long yz_ucmd_handler_begin(long cause);
extern "C" void yz_ucmd_handler_end(long cause, long epoch);
extern "C" uint8_t* vm_base;
#if defined(YZ_PPU_SAMPLE)
extern "C" void yz_ppu_perf_window_begin(uint32_t guest_frame);
extern "C" void yz_ppu_perf_window_dump(uint32_t guest_frame);
#endif
#if defined(YZ_PERF_PROFILE)
extern "C" void spu_perf_window_begin(uint32_t guest_frame);
extern "C" void spu_perf_window_dump(uint32_t guest_frame);
#endif
extern "C" void yz_a010_reltrace_ppu(uint32_t pc,
                                      const ppu_context* ctx);
extern "C" void yz_watch_arm_read(uint32_t guest_addr);
extern "C" void yz_stage_house_external_snapshot(void);
extern "C" volatile uint32_t g_yz_stage_house_instance;
static volatile uint32_t g_yz_a010_open_request;

static bool addr_readable(uint32_t a)
{
    /* Guest main-memory allocations extend well above the loaded ELF (scene
     * objects in this boot live around 0x65xxxxxx), plus the D0 stack arena. */
    return (a >= 0x00010000u && a < 0xD0000000u) ||
           (a >= 0xD0000000u && a < 0xE0000000u);
}

static bool yz_a010_authored_visible(uint32_t object, float time)
{
    uint32_t node = addr_readable(object + 0x1Cu)
                  ? vm_read32(object + 0x1Cu) : 0u;
    for (unsigned i = 0; i < 16u && addr_readable(node + 0x20u); i++) {
        const uint32_t track = vm_read32(node + 0x4u);
        if (addr_readable(track + 0x1Cu)) {
            const uint32_t start_bits = vm_read32(track + 0x18u);
            const uint32_t end_bits = vm_read32(track + 0x1Cu);
            float start = 0.0f, end = 0.0f;
            memcpy(&start, &start_bits, sizeof(start));
            memcpy(&end, &end_bits, sizeof(end));
            if (time >= start && time < end)
                return true;
        }
        node = vm_read32(node + 0x20u);
    }
    return false;
}

static uint32_t yz_active_a010_root()
{
    constexpr uint32_t manager_global = 0x014EC864u;
    constexpr uint32_t controller_auth_off = 0x220u + 0x090u;
    const uint32_t manager = vm_read32(manager_global);
    if (!addr_readable(manager + controller_auth_off))
        return 0u;
    const uint32_t root = vm_read32(manager + controller_auth_off);
    return addr_readable(root + 0x210u) &&
           vm_read32(root + 0x100u) == 0x61303130u ? root : 0u;
}

extern "C" volatile long g_yz_a010_root_active;
extern "C" volatile long g_yz_a010_release_scene_active;
extern "C" volatile long g_yz_a010_animation_ready;
extern "C" volatile long g_yz_a010_stage_generation = 0;
extern "C" volatile uint32_t g_yz_a010_stage_record_ea[9] = {};
extern "C" volatile uint32_t g_yz_a010_stage_payload_ea[9] = {};
extern "C" volatile uint32_t g_yz_a010_stage_result_ea[9];
extern "C" volatile long g_yz_a010_stage_result_get_mask;
extern "C" volatile uint32_t g_yz_a010_cmd_batch_start = 0;
extern "C" volatile uint32_t g_yz_a010_cmd_batch_end = 0;
extern "C" volatile uint32_t g_yz_a010_cmd_batch_count = 0;

/*
 * AUTH starts the orphanage clock before its asynchronous character
 * controllers have published their first authored palettes.  The temporary
 * controller pose is intentionally parked at Y=10000; advancing hundreds of
 * frames while that pose is live permanently desynchronizes animation,
 * camera, and model submission.  Recognize the first real pose only after it
 * has traversed both the 4x4 model matrix buffer and the packed 3x4 palette.
 */
static bool yz_a010_character_animation_ready(uint32_t object)
{
    if (!addr_readable(object + 0x2Cu) ||
        vm_read32(object) != 0x011D3F78u)
        return false;

    const uint32_t model = vm_read32(object + 0x2Cu);
    if (!addr_readable(model + 0x218u))
        return false;

    const uint32_t count = vm_read32(model + 0x210u);
    const uint32_t matrices = vm_read32(model + 0x214u);
    const uint32_t palette = vm_read32(model + 0x218u);
    if (count == 0u || count > 512u ||
        !addr_readable(matrices + 0x34u) ||
        !addr_readable(palette + 0x1Cu))
        return false;

    uint32_t matrix_y_bits = vm_read32(matrices + 0x34u);
    uint32_t palette_y_bits = vm_read32(palette + 0x1Cu);
    float matrix_y = 0.0f;
    float palette_y = 0.0f;
    memcpy(&matrix_y, &matrix_y_bits, sizeof(matrix_y));
    memcpy(&palette_y, &palette_y_bits, sizeof(palette_y));
    return std::isfinite(matrix_y) && std::isfinite(palette_y) &&
           std::fabs(matrix_y) < 1000.0f &&
           std::fabs(palette_y) < 1000.0f &&
           std::fabs(matrix_y - palette_y) < 64.0f;
}

/*
 * a010's character controller publishes one row-major 4x4 matrix per bone at
 * model+0x214. The renderer consumes three column vectors per bone from the
 * transient palette at model+0x218. Normally the model path combines the
 * current matrix with the bone's inverse-bind transform before pass 0.
 *
 * In the broken orphanage run the current matrices are valid during the
 * children's opening shot, but the transient palettes remain in the Y=10000
 * parking pose. Learn the inverse-bind transform from the game's own initial
 * parked matrix/palette pair, then use it only when an authored matrix is
 * paired with a still-parked palette.
 */
constexpr unsigned yz_a010_palette_models_max = 16u;
constexpr unsigned yz_a010_palette_bones_max = 512u;

struct yz_a010_palette_binding {
    uint32_t model;
    unsigned count;
    bool captured;
    bool repair_active;
    uint8_t valid[yz_a010_palette_bones_max];
    float inverse_bind[yz_a010_palette_bones_max][16];
};

static yz_a010_palette_binding
    g_yz_a010_palette_bindings[yz_a010_palette_models_max] = {};

static float yz_read_float(uint32_t address)
{
    const uint32_t bits = vm_read32(address);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void yz_write_float(uint32_t address, float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    vm_write32(address, bits);
}

static bool yz_matrix_finite(const float matrix[16])
{
    for (unsigned i = 0; i < 16u; ++i) {
        if (!std::isfinite(matrix[i]) ||
            std::fabs(matrix[i]) > 1000000.0f)
            return false;
    }
    return true;
}

static void yz_matrix_multiply(const float left[16],
                               const float right[16],
                               float result[16])
{
    float temporary[16] = {};
    for (unsigned row = 0; row < 4u; ++row) {
        for (unsigned column = 0; column < 4u; ++column) {
            float value = 0.0f;
            for (unsigned inner = 0; inner < 4u; ++inner)
                value += left[row * 4u + inner] *
                         right[inner * 4u + column];
            temporary[row * 4u + column] = value;
        }
    }
    memcpy(result, temporary, sizeof(temporary));
}

static bool yz_matrix_inverse(const float source[16], float inverse[16])
{
    double augmented[4][8] = {};
    for (unsigned row = 0; row < 4u; ++row) {
        for (unsigned column = 0; column < 4u; ++column)
            augmented[row][column] = source[row * 4u + column];
        augmented[row][row + 4u] = 1.0;
    }

    for (unsigned column = 0; column < 4u; ++column) {
        unsigned pivot = column;
        for (unsigned row = column + 1u; row < 4u; ++row) {
            if (std::fabs(augmented[row][column]) >
                std::fabs(augmented[pivot][column]))
                pivot = row;
        }
        if (std::fabs(augmented[pivot][column]) < 1.0e-10)
            return false;
        if (pivot != column) {
            for (unsigned i = 0; i < 8u; ++i) {
                const double value = augmented[column][i];
                augmented[column][i] = augmented[pivot][i];
                augmented[pivot][i] = value;
            }
        }
        const double divisor = augmented[column][column];
        for (unsigned i = 0; i < 8u; ++i)
            augmented[column][i] /= divisor;
        for (unsigned row = 0; row < 4u; ++row) {
            if (row == column)
                continue;
            const double factor = augmented[row][column];
            for (unsigned i = 0; i < 8u; ++i)
                augmented[row][i] -= factor * augmented[column][i];
        }
    }

    for (unsigned row = 0; row < 4u; ++row) {
        for (unsigned column = 0; column < 4u; ++column)
            inverse[row * 4u + column] =
                (float)augmented[row][column + 4u];
    }
    return yz_matrix_finite(inverse);
}

static void yz_read_bone_matrix(uint32_t matrices, unsigned bone,
                                float matrix[16])
{
    const uint32_t source = matrices + bone * 0x40u;
    for (unsigned i = 0; i < 16u; ++i)
        matrix[i] = yz_read_float(source + i * 4u);
}

static void yz_read_bone_palette(uint32_t palette, unsigned bone,
                                 float matrix[16])
{
    memset(matrix, 0, sizeof(float) * 16u);
    const uint32_t source = palette + bone * 0x30u;
    for (unsigned column = 0; column < 3u; ++column) {
        for (unsigned row = 0; row < 4u; ++row) {
            matrix[row * 4u + column] =
                yz_read_float(source + column * 0x10u + row * 4u);
        }
    }
    matrix[15] = 1.0f;
}

static void yz_write_bone_palette(uint32_t palette, unsigned bone,
                                  const float matrix[16])
{
    const uint32_t destination = palette + bone * 0x30u;
    for (unsigned column = 0; column < 3u; ++column) {
        for (unsigned row = 0; row < 4u; ++row) {
            yz_write_float(destination + column * 0x10u + row * 4u,
                           matrix[row * 4u + column]);
        }
    }
}

static yz_a010_palette_binding*
yz_a010_palette_binding_for(uint32_t model, unsigned count)
{
    yz_a010_palette_binding* empty = nullptr;
    for (yz_a010_palette_binding& binding : g_yz_a010_palette_bindings) {
        if (binding.model == model)
            return binding.count == count ? &binding : nullptr;
        if (!binding.model && !empty)
            empty = &binding;
    }
    if (!empty)
        return nullptr;
    empty->model = model;
    empty->count = count;
    return empty;
}

static void yz_a010_publish_character_palette(uint32_t object,
                                              uint32_t root)
{
    if (!addr_readable(object + 0x2Cu) ||
        vm_read32(object) != 0x011D3F78u)
        return;
    const uint32_t model = vm_read32(object + 0x2Cu);
    if (!addr_readable(model + 0x218u))
        return;
    const unsigned count = vm_read32(model + 0x210u);
    const uint32_t matrices = vm_read32(model + 0x214u);
    const uint32_t palette = vm_read32(model + 0x218u);
    if (!count || count > yz_a010_palette_bones_max ||
        !addr_readable(matrices) || !addr_readable(palette))
        return;

    yz_a010_palette_binding* binding =
        yz_a010_palette_binding_for(model, count);
    if (!binding)
        return;

    const float matrix_y = yz_read_float(matrices + 0x34u);
    const float palette_y = yz_read_float(palette + 0x1Cu);
    const bool matrix_parked =
        std::isfinite(matrix_y) && std::fabs(matrix_y) >= 1000.0f;
    const bool palette_parked =
        std::isfinite(palette_y) && std::fabs(palette_y) >= 1000.0f;
    const bool current_first =
        getenv("YZ_A010_PALETTE_CURRENT_FIRST") != nullptr;

    if (!binding->captured) {
        if (!matrix_parked || !palette_parked ||
            std::fabs(matrix_y - palette_y) >= 64.0f)
            return;
        unsigned valid_count = 0;
        for (unsigned bone = 0; bone < count; ++bone) {
            float initial_matrix[16] = {};
            float initial_palette[16] = {};
            float inverse_matrix[16] = {};
            yz_read_bone_matrix(matrices, bone, initial_matrix);
            yz_read_bone_palette(palette, bone, initial_palette);
            if (!yz_matrix_finite(initial_matrix) ||
                !yz_matrix_finite(initial_palette) ||
                !yz_matrix_inverse(initial_matrix, inverse_matrix))
                continue;
            /*
             * The parked pair alone cannot distinguish the two multiplication
             * conventions: both candidates reproduce that initial palette.
             * Keep the measured row-vector path as the default, but allow the
             * opposite order for a bounded rendered-frame comparison.
             */
            if (current_first) {
                yz_matrix_multiply(inverse_matrix, initial_palette,
                                   binding->inverse_bind[bone]);
            } else {
                yz_matrix_multiply(initial_palette, inverse_matrix,
                                   binding->inverse_bind[bone]);
            }
            if (!yz_matrix_finite(binding->inverse_bind[bone]))
                continue;
            binding->valid[bone] = 1u;
            ++valid_count;
        }
        binding->captured = valid_count != 0u;
        static unsigned long captures;
        ++captures;
        fprintf(stderr,
                "[a010-palette-binding] n=%lu object=%08X model=%08X "
                "bones=%u valid=%u matrixY=%.6f paletteY=%.6f "
                "order=%s\n",
                captures, object, model, count, valid_count,
                (double)matrix_y, (double)palette_y,
                current_first ? "current*inverseBind"
                              : "inverseBind*current");
        fflush(stderr);
        return;
    }

    /*
     * Once this fallback has replaced a parked palette, keep publishing from
     * the current matrices on subsequent frames.  Testing only for the parked
     * sentinel again would mistake our own first repaired result for native
     * output and freeze every actor at that one pose.
     */
    if (!std::isfinite(matrix_y) || std::fabs(matrix_y) >= 1000.0f)
        return;
    if (palette_parked)
        binding->repair_active = true;
    if (!binding->repair_active)
        return;

    unsigned published = 0;
    for (unsigned bone = 0; bone < count; ++bone) {
        if (!binding->valid[bone])
            continue;
        float current_matrix[16] = {};
        float current_palette[16] = {};
        yz_read_bone_matrix(matrices, bone, current_matrix);
        if (!yz_matrix_finite(current_matrix))
            continue;
        if (current_first) {
            yz_matrix_multiply(current_matrix, binding->inverse_bind[bone],
                               current_palette);
        } else {
            yz_matrix_multiply(binding->inverse_bind[bone], current_matrix,
                               current_palette);
        }
        if (!yz_matrix_finite(current_palette))
            continue;
        yz_write_bone_palette(palette, bone, current_palette);
        ++published;
    }

    static unsigned long repairs;
    ++repairs;
    if (repairs <= 32u || (repairs & (repairs - 1u)) == 0u) {
        uint32_t timeline_bits = vm_read32(root + 0x210u);
        float timeline = 0.0f;
        memcpy(&timeline, &timeline_bits, sizeof(timeline));
        fprintf(stderr,
                "[a010-palette-publication-repair] n=%lu object=%08X "
                "model=%08X time=%.3f bones=%u matrixY=%.6f "
                "paletteY=%.6f->%.6f order=%s\n",
                repairs, object, model, (double)timeline, published,
                (double)matrix_y, (double)palette_y,
                (double)yz_read_float(palette + 0x1Cu),
                current_first ? "current*inverseBind"
                              : "inverseBind*current");
        fflush(stderr);
    }
}

/*
 * Direct-call profiler for the a010 character-model path.
 *
 * The lifted renderer calls its pass routines and func_00094DA0 directly, so
 * ps3_indirect_call cannot observe those checkpoints.  MSVC /Gh instruments
 * only recomp/ppu_recomp_000.cpp; this hook then counts entries for the small
 * set below.  It intentionally performs no I/O (and calls no helpers) because
 * _penter runs before an instrumented function body.  The normal dispatcher
 * prints changed counters at the next func_00015480 invocation.
 */
struct yz_a010_direct_probe {
    uint32_t guest;
    void* host;
    volatile long hits;
};

static yz_a010_direct_probe g_yz_a010_direct_probes[] = {
    {0x00015480u, nullptr, 0}, /* AUTH character render dispatch */
    {0x00094288u, nullptr, 0}, /* model initialization wrapper */
    {0x00092CE0u, nullptr, 0}, /* model section initialization */
    {0x00095580u, nullptr, 0}, /* pass 0 */
    {0x00095E40u, nullptr, 0}, /* pass 8/9 */
    {0x000963C0u, nullptr, 0}, /* pass 1 */
    {0x000970C8u, nullptr, 0}, /* pass 4 */
    {0x00097268u, nullptr, 0}, /* pass 5 */
    {0x000979E8u, nullptr, 0}, /* pass 2/3 variant */
    {0x00097B28u, nullptr, 0}, /* pass 2/3 variant */
    {0x00097D00u, nullptr, 0}, /* pass 6 */
    {0x000984E8u, nullptr, 0}, /* pass 0xB */
    {0x00098AE0u, nullptr, 0}, /* pass 0xC */
    {0x00083780u, nullptr, 0}, /* pass 7 */
    {0x00094DA0u, nullptr, 0}, /* prepared-model section walker */
    {0x000A54A4u, nullptr, 0}, /* material/model section handoff */
    {0x000DBC2Cu, nullptr, 0}, /* renderer callback selector */
};
static volatile long g_yz_a010_direct_probe_enabled = 0;
static volatile long g_yz_stage_direct_probe_enabled = 0;

extern "C" void __declspec(noinline) _penter(void)
{
    if (!g_yz_a010_direct_probe_enabled ||
        (!g_yz_a010_root_active && !g_yz_stage_direct_probe_enabled))
        return;

    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    for (auto& probe : g_yz_a010_direct_probes) {
        const uintptr_t begin = (uintptr_t)probe.host;
        if (begin && ret >= begin && ret < begin + 0x100u) {
            _InterlockedIncrement(&probe.hits);
            return;
        }
    }
}

static void yz_a010_direct_probe_poll(uint32_t target)
{
    static int configured = 0;
    static long printed[
        sizeof(g_yz_a010_direct_probes) /
        sizeof(g_yz_a010_direct_probes[0])] = {};

    if (!configured) {
        configured = 1;
        const bool a010_enabled = getenv("YZ_A010_MODELREAD") != nullptr;
        const bool stage_enabled = getenv("YZ_STAGE_MODELREAD") != nullptr;
        if (!a010_enabled && !stage_enabled)
            return;
        for (auto& probe : g_yz_a010_direct_probes)
            probe.host = (void*)yz_lookup_func(probe.guest);
        _InterlockedExchange(&g_yz_a010_direct_probe_enabled, 1);
        if (stage_enabled)
            _InterlockedExchange(&g_yz_stage_direct_probe_enabled, 1);
        fprintf(stderr,
                "[model-direct] ARMED (%zu model-path functions, "
                "a010=%u stage=%u)\n",
                sizeof(g_yz_a010_direct_probes) /
                sizeof(g_yz_a010_direct_probes[0]),
                a010_enabled ? 1u : 0u, stage_enabled ? 1u : 0u);
        fflush(stderr);
    }
    if (!g_yz_a010_direct_probe_enabled ||
        !g_yz_a010_root_active ||
        target != 0x00015480u)
        return;

    for (unsigned i = 0;
         i < sizeof(g_yz_a010_direct_probes) /
             sizeof(g_yz_a010_direct_probes[0]);
         ++i) {
        const long hits = _InterlockedCompareExchange(
            &g_yz_a010_direct_probes[i].hits, 0, 0);
        if (hits != printed[i]) {
            fprintf(stderr, "[a010-direct] func_%08X hits=%ld (+%ld)\n",
                    g_yz_a010_direct_probes[i].guest,
                    hits, hits - printed[i]);
            printed[i] = hits;
        }
    }
    fflush(stderr);
}

extern "C" void yz_stage_direct_probe_snapshot(const char* reason)
{
    if (!g_yz_stage_direct_probe_enabled)
        return;

    static long printed[
        sizeof(g_yz_a010_direct_probes) /
        sizeof(g_yz_a010_direct_probes[0])] = {};
    bool header = false;
    for (unsigned i = 0;
         i < sizeof(g_yz_a010_direct_probes) /
             sizeof(g_yz_a010_direct_probes[0]);
         ++i) {
        const long hits = _InterlockedCompareExchange(
            &g_yz_a010_direct_probes[i].hits, 0, 0);
        if (hits == printed[i])
            continue;
        if (!header) {
            fprintf(stderr, "[stage-direct] SNAPSHOT reason=%s\n",
                    reason ? reason : "?");
            header = true;
        }
        fprintf(stderr,
                "[stage-direct] func_%08X hits=%ld (+%ld)\n",
                g_yz_a010_direct_probes[i].guest, hits,
                hits - printed[i]);
        printed[i] = hits;
    }
    if (header)
        fflush(stderr);
}

/* The New Game controller (func_004FFEE0) reads the orphanage AUTH object it
 * waits on from manager + 0x220 + 0x90.  The older probe watched
 * func_00BEC570's manager + 0x220 + 0x94 slot instead -- one word past the
 * live a010 object.  Poll the controller-owned slot and its three companion
 * objects while the bounded renderer/root-cause interval is armed.  This
 * follows the game's own status path without a boot-specific heap address. */
extern "C" void yz_a010_auth_probe_poll(void)
{
    if (!YZ_DIAG_ENABLED("YZ_A010_TRACE"))
        return;
    constexpr uint32_t manager_global = 0x014EC864u;
    constexpr uint32_t controller_auth_off = 0x220u + 0x090u;
    constexpr uint32_t controller_aux0_off = 0x220u + 0x290u;
    constexpr uint32_t controller_aux1_off = 0x220u + 0x224u;
    constexpr uint32_t controller_aux2_off = 0x220u + 0x3A4u;
    constexpr uint32_t active_owner_off = 0x220u + 0x110u;
    static int was_active = 0;
    static uint32_t prior_manager = ~0u;
    static uint32_t prior_object = 0;
    static uint32_t prior_raw = ~0u;
    static uint32_t prior_aux0 = ~0u;
    static uint32_t prior_aux1 = ~0u;
    static uint32_t prior_aux2 = ~0u;
    static uint32_t prior_owner = ~0u;
    static uint32_t prior_owner_state = ~0u;
    static uint32_t prior_player = ~0u;
    static unsigned poll_count = 0;
    const int active = rsx_live_draw_a010_probe_active() ||
                       g_yz_a010_root_active != 0;
    if (!active) {
        was_active = 0;
        return;
    }
    if (!was_active) {
        was_active = 1;
        prior_manager = ~manager_global;
        prior_object = 0;
        prior_raw = ~0u;
        prior_aux0 = prior_aux1 = prior_aux2 = ~0u;
        prior_owner = prior_owner_state = prior_player = ~0u;
        poll_count = 0;
    }

    const uint32_t manager = vm_read32(manager_global);
    const uint32_t object = addr_readable(manager) &&
                            addr_readable(manager + controller_auth_off)
                          ? vm_read32(manager + controller_auth_off) : 0;
    const uint32_t aux0 = addr_readable(manager) &&
                          addr_readable(manager + controller_aux0_off)
                        ? vm_read32(manager + controller_aux0_off) : 0;
    const uint32_t aux1 = addr_readable(manager) &&
                          addr_readable(manager + controller_aux1_off)
                        ? vm_read32(manager + controller_aux1_off) : 0;
    const uint32_t aux2 = addr_readable(manager) &&
                          addr_readable(manager + controller_aux2_off)
                        ? vm_read32(manager + controller_aux2_off) : 0;
    const uint32_t owner = addr_readable(manager) &&
                           addr_readable(manager + active_owner_off)
                         ? vm_read32(manager + active_owner_off) : 0;
    const uint32_t owner_state = addr_readable(owner + 0x104u)
                               ? vm_read32(owner + 0x104u) : ~0u;
    const uint32_t player = addr_readable(owner + 0xFCu)
                          ? vm_read32(owner + 0xFCu) : 0;
    const uint32_t raw = addr_readable(object) &&
                         addr_readable(object + 0xFCu)
                       ? vm_read32(object + 0xFCu) : ~0u;
    poll_count++;
    if (addr_readable(manager + 0x5C4u) &&
        (poll_count == 1u || poll_count == 32u ||
         poll_count == 128u || poll_count == 512u)) {
        fprintf(stderr,
                "[a010-manager] poll=%u manager=%08X "
                "legacy(+2B4)=%08X auth(+2B0)=%08X "
                "aux0(+4B0)=%08X aux1(+444)=%08X aux2(+5C4)=%08X\n",
                poll_count, manager, vm_read32(manager + 0x2B4u),
                object, aux0, aux1, aux2);
        fflush(stderr);
    }
    {
        static int handoff = -1;
        if (handoff < 0)
            handoff = getenv("YZ_A010_HANDOFF") ? 1 : 0;
        const bool milestone = poll_count <= 8u ||
                               (poll_count & (poll_count - 1u)) == 0u;
        if (handoff &&
            (milestone || owner != prior_owner ||
             owner_state != prior_owner_state || player != prior_player)) {
            fprintf(stderr,
                    "[a010-handoff] poll=%u manager=%08X auth=%08X "
                    "owner(+330)=%08X vtable=%08X state104=%s%u "
                    "playerFC=%08X owner214=%08X/%08X "
                    "clock21C=%08X repeats220=%08X "
                    "player0=%08X player40=%08X player50=%08X\n",
                    poll_count, manager, object, owner,
                    addr_readable(owner) ? vm_read32(owner) : 0u,
                    owner_state == ~0u ? "unreadable/" : "",
                    owner_state == ~0u ? 0u : owner_state,
                    player,
                    addr_readable(owner + 0x218u)
                        ? vm_read32(owner + 0x214u) : 0u,
                    addr_readable(owner + 0x218u)
                        ? vm_read32(owner + 0x218u) : 0u,
                    addr_readable(owner + 0x21Cu)
                        ? vm_read32(owner + 0x21Cu) : 0u,
                    addr_readable(owner + 0x220u)
                        ? vm_read32(owner + 0x220u) : 0u,
                    addr_readable(player) ? vm_read32(player) : 0u,
                    addr_readable(player + 0x40u)
                        ? vm_read32(player + 0x40u) : 0u,
                    addr_readable(player + 0x50u)
                        ? vm_read32(player + 0x50u) : 0u);
            fflush(stderr);
        }
    }
    prior_owner = owner;
    prior_owner_state = owner_state;
    prior_player = player;
    if (manager == prior_manager && object == prior_object && raw == prior_raw &&
        aux0 == prior_aux0 && aux1 == prior_aux1 && aux2 == prior_aux2)
        return;
    prior_manager = manager;
    prior_object = object;
    prior_raw = raw;
    prior_aux0 = aux0;
    prior_aux1 = aux1;
    prior_aux2 = aux2;
    fprintf(stderr,
            "[a010-auth] manager=%08X object=%08X raw=%s%u "
            "aux0=%08X aux1=%08X aux2=%08X tid=%u\n",
            manager, object, raw == ~0u ? "unreadable/" : "",
            raw == ~0u ? 0u : raw, aux0, aux1, aux2,
            yz_thread_current_id());
    if (addr_readable(object) && addr_readable(object + 0x11Cu)) {
        fprintf(stderr,
                "[a010-auth-obj] %08X:"
                " %08X %08X %08X %08X %08X %08X %08X %08X"
                " %08X %08X %08X %08X %08X %08X %08X %08X\n",
                object,
                vm_read32(object + 0x00u), vm_read32(object + 0x04u),
                vm_read32(object + 0x08u), vm_read32(object + 0x0Cu),
                vm_read32(object + 0x10u), vm_read32(object + 0x14u),
                vm_read32(object + 0x18u), vm_read32(object + 0x1Cu),
                vm_read32(object + 0xE0u), vm_read32(object + 0xE4u),
                vm_read32(object + 0xE8u), vm_read32(object + 0xECu),
                vm_read32(object + 0xF0u), vm_read32(object + 0xF4u),
                vm_read32(object + 0xF8u), vm_read32(object + 0xFCu));
        fprintf(stderr,
                "[a010-auth-tail] %08X:"
                " %08X %08X %08X %08X %08X %08X %08X %08X\n",
                object,
                vm_read32(object + 0x100u), vm_read32(object + 0x104u),
                vm_read32(object + 0x108u), vm_read32(object + 0x10Cu),
                vm_read32(object + 0x110u), vm_read32(object + 0x114u),
                vm_read32(object + 0x118u), vm_read32(object + 0x11Cu));

        /*
         * The AUTH state machine keeps the loaded track objects in +0x3B0
         * (count at +0x3B4) and the per-shot binding records in the vector at
         * +0x4C8.  The public state alone can advance normally even when one
         * of those objects never registers its render work, so preserve the
         * compact structural state at each transition.  This is gated by the
         * already opt-in a010 probe and deliberately caps both collections.
         */
        const uint32_t tracks = vm_read32(object + 0x3B0u);
        const uint32_t track_count = vm_read32(object + 0x3B4u);
        const uint32_t bindings = vm_read32(object + 0x4C8u);
        fprintf(stderr,
                "[a010-auth-core] raw=%u flags204=%08X frame208=%u "
                "time210=%08X/%08X active218=%08X "
                "play224=%08X/%08X/%08X/%08X "
                "flags280=%08X req288=%08X "
                "objects=%08X/%u bindings=%08X "
                "specials=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X "
                "tail=%08X/%08X\n",
                raw == ~0u ? 0u : raw,
                vm_read32(object + 0x204u), vm_read32(object + 0x208u),
                vm_read32(object + 0x210u), vm_read32(object + 0x214u),
                vm_read32(object + 0x218u),
                vm_read32(object + 0x224u), vm_read32(object + 0x228u),
                vm_read32(object + 0x22Cu), vm_read32(object + 0x230u),
                vm_read32(object + 0x280u), vm_read32(object + 0x288u),
                tracks, track_count, bindings,
                vm_read32(object + 0x39Cu), vm_read32(object + 0x3A0u),
                vm_read32(object + 0x3A4u), vm_read32(object + 0x3A8u),
                vm_read32(object + 0x3ACu), vm_read32(object + 0x3B8u),
                vm_read32(object + 0x3BCu), vm_read32(object + 0x4C4u),
                vm_read32(object + 0x4C8u), vm_read32(object + 0x52Cu));

        if ((raw == 3u || raw == 16u) &&
            addr_readable(tracks) && track_count > 0u) {
            const uint32_t cap = track_count < 64u ? track_count : 64u;
            for (uint32_t i = 0; i < cap; i++) {
                const uint32_t track = vm_read32(tracks + i * 4u);
                fprintf(stderr,
                        "[a010-auth-track] raw=%u i=%u object=%08X "
                        "vtable=%08X words=%08X/%08X/%08X/%08X "
                        "owned14=%08X/%08X/%08X owned2C=%08X/%08X "
                        "flagsDC=%08X\n",
                        raw, i, track,
                        addr_readable(track) ? vm_read32(track) : 0u,
                        addr_readable(track + 0x4u)
                            ? vm_read32(track + 0x4u) : 0u,
                        addr_readable(track + 0x8u)
                            ? vm_read32(track + 0x8u) : 0u,
                        addr_readable(track + 0xCu)
                            ? vm_read32(track + 0xCu) : 0u,
                        addr_readable(track + 0x10u)
                            ? vm_read32(track + 0x10u) : 0u,
                        addr_readable(track + 0x14u)
                            ? vm_read32(track + 0x14u) : 0u,
                        addr_readable(track + 0x18u)
                            ? vm_read32(track + 0x18u) : 0u,
                        addr_readable(track + 0x1Cu)
                            ? vm_read32(track + 0x1Cu) : 0u,
                        addr_readable(track + 0x2Cu)
                            ? vm_read32(track + 0x2Cu) : 0u,
                        addr_readable(track + 0x30u)
                            ? vm_read32(track + 0x30u) : 0u,
                        addr_readable(track + 0xDCu)
                            ? vm_read32(track + 0xDCu) : 0u);
            }
        }
        if ((raw == 3u || raw == 16u) && addr_readable(bindings) &&
            addr_readable(bindings + 0xCu)) {
            const uint32_t data = vm_read32(bindings + 0x4u);
            const uint32_t count = vm_read32(bindings + 0x8u);
            fprintf(stderr,
                    "[a010-auth-bindings] raw=%u vector=%08X "
                    "data=%08X count=%u capacity=%u\n",
                    raw, bindings, data, count,
                    vm_read32(bindings + 0xCu));
            const uint32_t cap = count < 16u ? count : 16u;
            for (uint32_t i = 0; i < cap; i++) {
                const uint32_t rec = data + i * 0x80u;
                if (!addr_readable(rec + 0x1Cu)) break;
                fprintf(stderr,
                        "[a010-auth-binding] raw=%u i=%u rec=%08X "
                        "words=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X\n",
                        raw, i, rec,
                        vm_read32(rec + 0x00u), vm_read32(rec + 0x04u),
                        vm_read32(rec + 0x08u), vm_read32(rec + 0x0Cu),
                        vm_read32(rec + 0x10u), vm_read32(rec + 0x14u),
                        vm_read32(rec + 0x18u), vm_read32(rec + 0x1Cu));
            }
        }
    }
    fflush(stderr);
}

/* Timestamp shared with the unattended New Game acceptance input route. */
extern "C" volatile unsigned long long g_yz_auto_start_tick = 0;
extern "C" volatile long g_yz_a010_root_active;
static uint32_t g_yz_auto_input_cached;
static uint32_t g_yz_auto_input_mask;

/* One-shot diagnostic Start press at the title callback. The game does not
 * read cellPadGetData here; it consumes two cached button bitsets owned by its
 * input manager. Injecting both copies immediately before func_00AF1DD0 runs
 * follows the same title-state-machine path as a real Start press. */
static void yz_title_auto_start(ppu_context* ctx, uint32_t address)
{
    static int state;
    static std::chrono::steady_clock::time_point deadline;
    const auto guest_readable = [](uint32_t a) {
        return (a >= 0x00010000u && a < 0xD0000000u) ||
               (a >= 0xD0000000u && a < 0xE0000000u);
    };

    if (!g_yz_runtime_config.auto_start ||
        state == 2 || address != 0x00AF1DD0u) return;

    if (state == 0) {
        state = 1;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        fprintf(stderr, "[auto-start] title input callback reached; injecting START in 3 seconds\n");
        fflush(stderr);
        return;
    }
    if (std::chrono::steady_clock::now() < deadline) return;

    const uint32_t title = (uint32_t)ctx->gpr[3];
    if (!guest_readable(title) || !guest_readable(title + 0x13Cu)) return;
    const uint32_t input = vm_read32(title + 0x13Cu);
    if (!guest_readable(input) || !guest_readable(input + 0x8u)) return;
    const uint32_t owner = vm_read32(input + 0x8u);
    if (!guest_readable(owner) || !guest_readable(owner + 0xE70u)) return;
    const uint32_t cached = owner + 0xB68u;
    if (!guest_readable(cached + 0x308u)) return;
    const uint32_t mask = vm_read32(cached + 0x308u);
    if (!guest_readable(mask)) return;

    vm_write32(mask, vm_read32(mask) | 0x100u);
    vm_write32(cached + 0x8u, vm_read32(cached + 0x8u) | 0x100u);
    g_yz_auto_input_cached = cached;
    g_yz_auto_input_mask = mask;
    g_yz_auto_start_tick = GetTickCount64();
    state = 2;
    fprintf(stderr,
            "[auto-start] injected cached START title=%08X input=%08X cached=%08X mask=%08X\n",
            title, input, cached, mask);
    fflush(stderr);
}

/* The title/menu state machine consumes the game's cached input abstraction,
 * not cellPadGetData directly.  Keep the unattended New Game route on that
 * same authoritative seam as the Start injection above.  The menu handler
 * func_00AF24A4 reads the edge set at cached+4.  Its confirm action mask is
 * stored at 0x014EE698 (0x014EE6A0 is the separate Back/Cancel action).
 * Read the live mapping rather than assuming a fixed Cross/Circle layout.
 * Title Start is a different cached+8 / 0x100 contract.  This remains
 * diagnostic-only and stops as soon as the a010 root marker is active. */
static void yz_auto_new_game_cached_input()
{
    static unsigned long long logged;
    if (!g_yz_runtime_config.auto_new_game ||
        !g_yz_auto_start_tick || !g_yz_auto_input_cached ||
        !g_yz_auto_input_mask || g_yz_a010_root_active)
        return;

    const unsigned long long elapsed =
        GetTickCount64() - g_yz_auto_start_tick;
    const unsigned long long first = 12000u;
    const unsigned long long period = 3000u;
    const unsigned attempts = 5u;
    if (elapsed < first || elapsed >= first + period * attempts)
        return;

    const unsigned pulse = (unsigned)((elapsed - first) / period);
    const unsigned long long phase = (elapsed - first) % period;
    const uint32_t accept_bit =
        addr_readable(0x014EE698u) ? vm_read32(0x014EE698u) : 0x1u;
    /*
     * Hold Confirm for two seconds, then release for one.  The recompilation
     * can run below 10 FPS during scene setup, so the old 500 ms tap was
     * occasionally created and cleared entirely between consumers.  Do not
     * inject Up here: after the first missed Confirm it walked the highlight
     * away from New Game and made later retries target unrelated rows.
     */
    const uint32_t bit = phase < 2000u ? accept_bit : 0u;
    const uint32_t edge = g_yz_auto_input_cached + 0x4u;
    vm_write32(g_yz_auto_input_mask,
               vm_read32(g_yz_auto_input_mask) &
               ~accept_bit);
    vm_write32(edge, vm_read32(edge) & ~accept_bit);
    if (bit) {
        vm_write32(g_yz_auto_input_mask,
                   vm_read32(g_yz_auto_input_mask) | bit);
        vm_write32(edge, vm_read32(edge) | bit);
        if (bit == accept_bit && pulse < 64u &&
            !(logged & (1ull << pulse))) {
            logged |= 1ull << pulse;
            fprintf(stderr,
                    "[auto-new-game] cached Confirm pulse %u/%u at +%llums\n",
                    pulse + 1u, attempts, elapsed);
            fflush(stderr);
        }
    }
}

/* Inject the accept edge into the cache owned by the menu object that is
 * actually entering func_00AF24A4.  The title prompt and main menu normally
 * share the same input service, but they do not necessarily retain the same
 * owner/cache instance across the title-to-menu transition.  Calling this
 * immediately before the callback also prevents the normal input update from
 * clearing the synthetic edge before the menu consumes it. */
static void yz_auto_new_game_menu_input(ppu_context* ctx, uint32_t target)
{
    static unsigned long long first_seen;
    static bool selected_new_game;
    static unsigned logged_pulse = ~0u;
    if (!g_yz_runtime_config.auto_new_game ||
        g_yz_a010_root_active) return;

    const uint32_t menu = (uint32_t)ctx->gpr[3];
    if (!addr_readable(menu + 0x13Cu)) return;
    const uint32_t input = vm_read32(menu + 0x13Cu);
    if (!addr_readable(input + 0x8u)) return;
    const uint32_t owner = vm_read32(input + 0x8u);
    if (!addr_readable(owner + 0xE70u)) return;
    const uint32_t cached = owner + 0xB68u;
    if (!addr_readable(cached + 0x308u)) return;
    const uint32_t mask = vm_read32(cached + 0x308u);
    if (!addr_readable(mask)) return;

    const unsigned long long now = GetTickCount64();
    if (!first_seen) {
        first_seen = now;
        fprintf(stderr,
                "[auto-new-game] live menu input menu=%08X input=%08X cached=%08X mask=%08X"
                " (title cached=%08X); selecting New Game in 1.5 seconds\n",
                menu, input, cached, mask, g_yz_auto_input_cached);
        fflush(stderr);
        return;
    }
    if (!selected_new_game && now - first_seen >= 1500u) {
        /*
         * A save on disk makes Load Game the initially highlighted row.  The
         * menu keeps its item table at +0x150, count at +0x154, and selected
         * row at +0x17C.  Item id 0 is New Game.  Find it explicitly so this
         * remains correct if optional rows change the menu ordering.
         */
        const uint32_t count = vm_read32(menu + 0x154u);
        const uint32_t old_selection = vm_read32(menu + 0x17Cu);
        const uint32_t table = vm_read32(menu + 0x150u);
        uint32_t new_selection = 0xFFFFFFFFu;
        if (count > 0u && count < 64u && addr_readable(table)) {
            for (uint32_t i = 0; i < count; ++i) {
                if (addr_readable(table + i * 4u) &&
                    vm_read32(table + i * 4u) == 0u) {
                    new_selection = i;
                    break;
                }
            }
        }
        if (new_selection != 0xFFFFFFFFu && old_selection < count) {
            vm_write32(menu + 0x17Cu, new_selection);
            fprintf(stderr,
                    "[auto-new-game] menu row %u/%u -> %u/%u"
                    " (item 0 = New Game)\n",
                    old_selection, count, new_selection, count);
            fflush(stderr);
        } else {
            fprintf(stderr,
                    "[auto-new-game] New Game item unavailable"
                    " row=%u count=%u table=%08X\n",
                    old_selection, count, table);
            fflush(stderr);
        }
        selected_new_game = true;
    }
    if (now - first_seen < 3500u) return;

    /* 0x00AF24A4 is the code address and 0x01347AC8 is its OPD address; both
     * dispatch to the same callback.  Pulse one second on/two seconds off.
     * A permanent edge never gives the menu's cached-input state machine the
     * release transition it requires and can leave New Game highlighted
     * forever without activating it. */
    if (target != 0x00AF24A4u && target != 0x01347AC8u) return;
    const uint32_t bit =
        addr_readable(0x014EE698u) ? vm_read32(0x014EE698u) : 0x1u;
    const unsigned long long accept_elapsed = now - first_seen - 3500u;
    const unsigned pulse = (unsigned)(accept_elapsed / 3000u);
    const bool pressed = (accept_elapsed % 3000u) < 1000u;
    /*
     * Do not own the live menu input cache forever when the synthetic
     * selection/accept route fails.  An unbounded retry continuously clears
     * the real Confirm edge during each release phase, which makes the
     * visible menu impossible to operate manually.  Five attempts retain
     * unattended startup while handing the cache back after 15 seconds.
     */
    if (pulse >= 5u) {
        return;
    }
    if (pressed) {
        vm_write32(mask, vm_read32(mask) | bit);
        vm_write32(cached + 0x4u, vm_read32(cached + 0x4u) | bit);
    } else {
        vm_write32(mask, vm_read32(mask) & ~bit);
        vm_write32(cached + 0x4u, vm_read32(cached + 0x4u) & ~bit);
    }
    if (pressed && pulse != logged_pulse) {
        logged_pulse = pulse;
        fprintf(stderr,
                "[auto-new-game] pulsing live menu Accept %u"
                " target=%08X menu=%08X cached=%08X edge=%08X"
                " enabled=%08X\n",
                pulse + 1u, target, menu, cached,
                vm_read32(cached + 0x4u), vm_read32(mask));
        fflush(stderr);
    }
}

/* Opt-in observer for the title prompt/selector state machine.  The indirect
 * dispatcher below covers callback transitions without changing behavior;
 * local generated entry probes may reuse it when direct-call detail is needed. */
extern "C" void yz_title_trace(ppu_context* ctx, unsigned address)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("YZ_TITLE_TRACE") ? 1 : 0;
    if (!enabled) return;

    unsigned slot;
    switch (address) {
    case 0x00AF1FB0u: slot = 0; break;
    case 0x00AF24A4u: slot = 1; break;
    case 0x00AF2F40u: slot = 2; break;
    case 0x00AF4CA0u: slot = 3; break;
    case 0x00AF53DCu: slot = 4; break;
    case 0x00AF5614u: slot = 5; break;
    case 0x00AF56BCu: slot = 6; break;
    case 0x00AF5860u: slot = 7; break;
    case 0x00AF0E10u: slot = 8; break;
    case 0x00AF0FE4u: slot = 9; break;
    case 0x00AF11CCu: slot = 10; break;
    case 0x00AF13E4u: slot = 11; break;
    case 0x00AF1390u: slot = 12; break;
    case 0x00AF1DD0u: slot = 13; break;
    case 0x00AF1F48u: slot = 14; break;
    default: return;
    }

    const uint32_t obj = (uint32_t)ctx->gpr[3];
    if (obj < 0x10000u || obj >= 0xD0000000u) return;

    const uint32_t now[] = {
        vm_read32(obj + 0x138u), vm_read32(obj + 0x13Cu),
        vm_read32(obj + 0x148u), vm_read32(obj + 0x14Cu),
        vm_read32(obj + 0x150u), vm_read32(obj + 0x154u),
        vm_read32(obj + 0x17Cu), vm_read32(obj + 0x180u),
        vm_read32(obj + 0x188u), vm_read32(obj + 0x1A0u),
        vm_read32(obj + 0x1ACu), vm_read32(obj + 0x1B0u),
        vm_read32(obj + 0x1B4u), vm_read32(obj + 0x1B8u),
        vm_read32(obj + 0x1C0u), vm_read32(obj + 0x100u),
        vm_read32(obj + 0x104u), vm_read32(obj + 0x108u),
        vm_read32(obj + 0x10Cu), vm_read32(obj + 0x114u),
        vm_read32(obj + 0x118u), vm_read32(obj + 0x11Cu),
        vm_read32(obj + 0x120u), vm_read32(obj + 0x124u),
        vm_read32(obj + 0x128u), vm_read32(obj + 0x134u),
        vm_read32(obj + 0x140u), vm_read32(obj + 0x144u)
    };
    const uint32_t table = now[4];
    const uint32_t index = now[6];
    uint32_t selected = 0xFFFFFFFFu;
    uint32_t states[9] = {};
    if (table >= 0x10000u && table < 0xD0000000u && index < 0x1000u) {
        for (unsigned i = 0; i < 9; ++i)
            states[i] = vm_read32(table + i * 4u);
        selected = vm_read32(table + index * 4u);
    }

    static uint32_t last_obj[16];
    static uint32_t last[16][sizeof(now) / sizeof(now[0])];
    static uint32_t last_selected[16];
    static unsigned seen;
    static unsigned lines;
    static bool armed;
    if (!armed) {
        armed = true;
        fprintf(stderr, "[title-trace] direct observer armed\n");
    }

    bool changed = !(seen & (1u << slot)) || obj != last_obj[slot] ||
                   selected != last_selected[slot];
    for (unsigned i = 0; i < sizeof(now) / sizeof(now[0]); ++i)
        if (now[i] != last[slot][i]) changed = true;
    if (!changed || lines >= 2000) return;

    ++lines;
    fprintf(stderr,
            "[title-trace] fn=%08X obj=%08X ui=%08X input=%08X "
            "timer=%u handle=%08X table=%08X count=%u idx=%u selected=%u "
            "180=%u 188=%u 1a0=%u 1ac=%u 1b0=%u 1b4=%08X 1b8=%u 1c0=%u "
            "100=%u 104=%u 108=%u 10c=%u 114=%u 118=%u 11c=%u 120=%u "
            "124=%u 128=%u 134=%u 140=%08X 144=%08X "
            "states=[%u,%u,%u,%u,%u,%u,%u,%u,%u]\n",
            address, obj, now[0], now[1], now[2], now[3], now[4], now[5],
            now[6], selected, now[7], now[8], now[9], now[10], now[11],
            now[12], now[13], now[14], now[15], now[16], now[17], now[18],
            now[19], now[20], now[21], now[22], now[23], now[24], now[25],
            now[26], now[27], states[0], states[1], states[2],
            states[3], states[4], states[5], states[6], states[7], states[8]);
    fflush(stderr);
    seen |= 1u << slot;
    last_obj[slot] = obj;
    last_selected[slot] = selected;
    for (unsigned i = 0; i < sizeof(now) / sizeof(now[0]); ++i)
        last[slot][i] = now[i];
}

/* ===========================================================================
 * mwPly (CRI Sofdec player) dynamic-ABI probe (env YZ_MWPLY_PROBE, 2026-07-04)
 * See scratch/MWPLY_RESOLVE.md for the resolution recipe. Three resolved
 * addresses, all reached only via indirect (bctr) dispatch:
 *   func_00F4D0A8 = mwPlyIsNextFrmReady (the poll gate t1 spins on)
 *   func_00F4DA90 = mwPlyGetFrm         (decoded video frame)
 *   func_00F48E48 = mwPlyGetAudioPcmData_PS3 (audio PCM pull)
 * Diagnostic-only; gated OFF by default (band-aid hygiene: env-gated,
 * default-off, kill-switched). Also implements
 * YZ_MWPLY_FORCEREADY (separate flag, default OFF): skip the real
 * IsNextFrmReady call and force ctx->gpr[3]=1, to see whether the player then
 * advances into calling the getters at all.
 * =========================================================================*/
#define YZ_MWPLY_ISREADY   0x00F4D0A8u
#define YZ_MWPLY_GETFRM    0x00F4DA90u
#define YZ_MWPLY_GETAUDIO  0x00F48E48u

static bool yz_mwply_probe_target(uint32_t target)
{
    static int en = -1;
    if (en < 0) {
        en = getenv("YZ_MWPLY_PROBE") ? 1 : 0;
        if (en == 1) {
            fprintf(stderr, "[mwply] probe ARMED (3 targets)\n");
            fflush(stderr);
        }
    }
    if (!en) return false;
    return target == YZ_MWPLY_ISREADY || target == YZ_MWPLY_GETFRM ||
           target == YZ_MWPLY_GETAUDIO;
}

static const char* yz_mwply_name(uint32_t target)
{
    if (target == YZ_MWPLY_ISREADY)  return "mwPlyIsNextFrmReady";
    if (target == YZ_MWPLY_GETFRM)   return "mwPlyGetFrm";
    if (target == YZ_MWPLY_GETAUDIO) return "mwPlyGetAudioPcmData_PS3";
    return "?";
}

/* Volume-bounded gate shared by all three probe sites: first 40 calls, then
 * 1-in-500 (a tight poll must not flood/serialize the guest). */
static bool yz_mwply_should_log(long* counter)
{
    long c = ++(*counter);
    return c <= 40 || (c % 500) == 0;
}

/* Dump `len` bytes at guest EA `ea` as hex words, guest-BE (as the game wrote
 * them), guarded by addr_readable so a garbage pointer can't fault the probe. */
static void yz_mwply_dump_buf(const char* tag, uint32_t ea, uint32_t len)
{
    if (!ea) { fprintf(stderr, "  %s=NULL\n", tag); return; }
    if (!addr_readable(ea)) {
        fprintf(stderr, "  %s=0x%08X (UNREADABLE, skipped)\n", tag, ea);
        return;
    }
    fprintf(stderr, "  %s=0x%08X:", tag, ea);
    for (uint32_t o = 0; o < len; o += 4)
        fprintf(stderr, " %08X", vm_read32(ea + o));
    fprintf(stderr, "\n");
}

extern "C" void yz_drain_trampolines(ppu_context* ctx);

/* Entry point called from ps3_indirect_call for the three probe targets
 * instead of the normal g_trampoline_fn hand-off. We call the real function
 * SYNCHRONOUSLY here (rather than via the trampoline slot) so we can log its
 * return value and out-params right after -- safe because these are leaf-ish
 * CRI player calls, not long tail-chains that would grow the host stack. */
static void yz_mwply_probe_dispatch(uint32_t target, yz_ppu_fn fn, ppu_context* ctx)
{
    static long counters[3] = {0, 0, 0};
    int idx = (target == YZ_MWPLY_ISREADY) ? 0 : (target == YZ_MWPLY_GETFRM) ? 1 : 2;
    bool log_this = yz_mwply_should_log(&counters[idx]);
    const char* nm = yz_mwply_name(target);
    unsigned tid = yz_thread_current_id();

    uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4], r5 = ctx->gpr[5];
    uint64_t r6 = ctx->gpr[6], r7 = ctx->gpr[7], r8 = ctx->gpr[8];

    if (log_this) {
        fprintf(stderr, "[mwply] %s tid=%u r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX r7=0x%llX r8=0x%llX\n",
                nm, tid, (unsigned long long)r3, (unsigned long long)r4,
                (unsigned long long)r5, (unsigned long long)r6,
                (unsigned long long)r7, (unsigned long long)r8);
        fflush(stderr);
    }

    static int force_ready = -1;
    if (force_ready < 0) force_ready = getenv("YZ_MWPLY_FORCEREADY") ? 1 : 0;

    if (target == YZ_MWPLY_ISREADY && force_ready) {
        /* Skip the real poll test; force "ready" (nonzero) so we can see
         * whether the player advances to calling the getters at all. */
        ctx->gpr[3] = 1;
        if (log_this) {
            fprintf(stderr, "  [FORCEREADY] skipped real call, forced ret=1\n");
            fflush(stderr);
        }
    } else {
        fn(ctx);
        yz_drain_trampolines(ctx);
    }

    if (log_this) {
        fprintf(stderr, "  -> ret ctx->gpr[3]=0x%llX\n", (unsigned long long)ctx->gpr[3]);
        if (target == YZ_MWPLY_GETFRM) {
            /* r4 = small out-struct (init'd to -1 sentinel per static read);
             * r5 = ~0xA8-byte out-struct memset to 0 then filled. */
            yz_mwply_dump_buf("r4(small-struct)", (uint32_t)r4, 0x20);
            yz_mwply_dump_buf("r5(frame-struct,0xA8)", (uint32_t)r5, 0xA8);
        } else if (target == YZ_MWPLY_GETAUDIO) {
            /* r4/r5/r6 = the out pointers per the static ABI read (count/ptr
             * fields observed at entry); dump ~0x40 bytes at each candidate. */
            yz_mwply_dump_buf("r4", (uint32_t)r4, 0x40);
            yz_mwply_dump_buf("r5", (uint32_t)r5, 0x40);
            yz_mwply_dump_buf("r6", (uint32_t)r6, 0x40);
        }
        fflush(stderr);
    }
}

extern "C" yz_ppu_fn yz_lookup_func(uint32_t guest_addr)
{
    /* The generated table contains lifter artifacts outside any mappable code
     * window (entries at 0x0/0x7C/... and at 0xFExxxxxx-0xFFFFFFxx, the latter
     * shadowing the import fake-key range). A corrupted or null bctr target
     * that matches one would execute unrelated lifted code and every downstream
     * crash would be attributed to the wrong function. Valid guest code is
     * 4-aligned inside the game module (< 0x02000000) or the lifted PRX window
     * (0x02200000-0x03000000); everything else resolves to "unknown" so it
     * reaches the no-host-function net with the true target in the report. */
    /* PRX window: the native gate lifts firmware PRX at 0x0220xxxx+, the LLE
     * oracle at 0x0200xxxx+ — accept the union [0x02000000, 0x03000000). */
    if ((guest_addr & 3u) || guest_addr < 0x10000u ||
        guest_addr >= 0x03000000u) {
        static long rejected = 0;
        long n = _InterlockedIncrement(&rejected);
        if (n <= 8)
            fprintf(stderr,
                    "[dispatch] rejected out-of-range guest target 0x%08X "
                    "(#%ld; not game/PRX code)\n", guest_addr, n);
        return nullptr;
    }
    unsigned lo = 0, hi = g_yz_func_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        uint32_t a = g_yz_func_table[mid].addr;
        if (a == guest_addr) return g_yz_func_table[mid].fn;
        if (a < guest_addr)  lo = mid + 1;
        else                 hi = mid;
    }
    return nullptr;
}

/* The game module's TOC (set from the entry OPD in main). Used to repair a
 * lost TOC when a guest function is dispatched with r2==0 (see yz_tramp_guard). */
extern "C" uint32_t g_yz_game_toc = 0;

struct yz_a010_house_mesh_probe_state {
    uint32_t captured_sites;
    uint32_t group_slot;
    uint32_t mesh_index;
    uint32_t stage_object;
    uint32_t before_command_head;
    uint32_t before_command_count;
    bool active;
};

static thread_local yz_a010_house_mesh_probe_state
    g_yz_a010_house_mesh_probe_state = {};

struct yz_a010_building_pass_scope {
    uint32_t target;
    uint32_t stage_object;
    uint32_t command_arena;
    uint32_t first_record;
    uint32_t first_caller;
    uint32_t first_args[6];
    unsigned builder_calls;
    bool active;
};

static thread_local yz_a010_building_pass_scope
    g_yz_a010_building_pass_scope = {};

static bool g_yz_a010_stage_main_repaired = false;

extern "C" void yz_a010_common_builder_entry(void* ctxv)
{
    auto& scope = g_yz_a010_building_pass_scope;
    ppu_context* ctx = static_cast<ppu_context*>(ctxv);
    if (!scope.active || !YZ_DIAG_ENABLED("YZ_A010_HOUSE_MESH_TRACE")) {
        /*
         * The scene manager contains an inlined copy of FE5DB8 rather than
         * entering the virtual method. Count those direct EA1E78 calls after
         * the house-only ready-bit repair, using their exact shipped caller
         * range so character/model builders are excluded.
         */
        const uint32_t caller = (uint32_t)ctx->lr;
        if (g_yz_a010_stage_main_repaired &&
            YZ_DIAG_ENABLED("YZ_A010_HOUSE_MESH_TRACE") &&
            caller >= 0x00113484u &&
            caller <= 0x0011716Cu) {
            static unsigned inlined_builder_calls = 0u;
            ++inlined_builder_calls;
            if (inlined_builder_calls <= 8u ||
                (inlined_builder_calls &
                 (inlined_builder_calls - 1u)) == 0u) {
                fprintf(stderr,
                        "[a010-stage-main-builder] n=%u caller=%08X "
                        "r3=%08X r4=%08X r5=%08X r6=%08X "
                        "r7=%08X r8=%08X\n",
                        inlined_builder_calls, caller,
                        (uint32_t)ctx->gpr[3],
                        (uint32_t)ctx->gpr[4],
                        (uint32_t)ctx->gpr[5],
                        (uint32_t)ctx->gpr[6],
                        (uint32_t)ctx->gpr[7],
                        (uint32_t)ctx->gpr[8]);
                fflush(stderr);
            }
        }
        return;
    }

    const uint32_t command_head =
        addr_readable(scope.command_arena + 0x20u)
            ? vm_read32(scope.command_arena + 0x00u) : 0u;
    const uint32_t command_count =
        addr_readable(scope.command_arena + 0x20u)
            ? vm_read32(scope.command_arena + 0x1Cu) : 0u;
    ++scope.builder_calls;
    if (scope.builder_calls == 1u) {
        scope.first_record = command_head;
        scope.first_caller = (uint32_t)ctx->lr;
        for (unsigned i = 0u; i < 6u; ++i)
            scope.first_args[i] = (uint32_t)ctx->gpr[3u + i];
    }
    if (scope.builder_calls <= 8u) {
        fprintf(stderr,
                "[a010-building-builder] n=%u caller=%08X "
                "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X "
                "arena=%08X head=%08X count=%u\n",
                scope.builder_calls, (uint32_t)ctx->lr,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
                (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8],
                scope.command_arena, command_head, command_count);
        fflush(stderr);
    }
}

extern "C" void yz_a010_house_mesh_probe(
    void* ctxv, unsigned stage_object, unsigned group_slot,
    unsigned mesh_index, unsigned site, unsigned phase)
{
    if (!YZ_DIAG_ENABLED("YZ_A010_HOUSE_MESH_TRACE"))
        return;

    auto& probe = g_yz_a010_house_mesh_probe_state;
    ppu_context* ctx = static_cast<ppu_context*>(ctxv);

    if (phase == 0u) {
        probe.active = false;
        if (!addr_readable(stage_object + 0xF8u) ||
            vm_read32(stage_object + 0x04u) != 0x429D8A00u)
            return;

        const uint32_t flags = vm_read32(stage_object + 0x10u);
        const uint16_t mesh_flags =
            addr_readable(flags + mesh_index * 2u + 1u)
                ? vm_read16(flags + mesh_index * 2u) : 0xFFFFu;
        /* Select the first descriptor the active shipped pass will consume.
         * The shipped branches skip meshes when these pass bits are clear:
         * FE4508 requires 0x01/0x10 and FE8CA0 requires 0x100. */
        const uint16_t required_mask =
            site == 0x00FE8DD0u ? 0x0100u : 0x0011u;
        if ((mesh_flags & required_mask) == 0u)
            return;

        const uint32_t site_bit =
            site == 0x00FE466Cu ? 1u :
            site == 0x00FE4950u ? 2u :
            site == 0x00FE8DD0u ? 4u : 0u;
        if (!site_bit || (probe.captured_sites & site_bit))
            return;
        probe.captured_sites |= site_bit;
        probe.active = true;
        probe.group_slot = group_slot;
        probe.mesh_index = mesh_index;
        probe.stage_object = stage_object;

        const uint32_t gmd = vm_read32(stage_object + 0x04u);
        const uint32_t group_table =
            addr_readable(gmd + 0xF8u) ? vm_read32(gmd + 0xF4u) : 0u;
        const uint32_t list =
            addr_readable(group_table + 0x08u)
                ? vm_read32(group_table + 0x04u) : 0u;
        const uint32_t record_base =
            vm_read32(stage_object +
                      (site == 0x00FE4950u ? 0xF8u : 0xF4u));
        const uint32_t record = record_base + mesh_index * 12u;

        fprintf(stderr,
                "[a010-house-mesh] ENTRY site=%08X stage=%08X "
                "gmd=%08X table=%08X group=%u count=%u/%u list=%08X "
                "mesh=%u flags=%04X record=%08X [%04X,%04X,%04X,%08X]\n",
                site, stage_object, gmd, group_table, group_slot,
                addr_readable(group_table + 0x13u)
                    ? vm_read32(group_table + 0x00u) : 0u,
                addr_readable(group_table + 0x13u)
                    ? vm_read32(group_table + 0x10u) : 0u,
                list, mesh_index, (unsigned)mesh_flags, record,
                addr_readable(record + 0x0Bu)
                    ? (unsigned)vm_read16(record + 0x00u) : 0u,
                addr_readable(record + 0x0Bu)
                    ? (unsigned)vm_read16(record + 0x02u) : 0u,
                addr_readable(record + 0x0Bu)
                    ? (unsigned)vm_read16(record + 0x04u) : 0u,
                addr_readable(record + 0x0Bu)
                    ? vm_read32(record + 0x08u) : 0u);
        fflush(stderr);
        return;
    }

    if (!probe.active)
        return;

    const uint32_t command_arena =
        g_yz_game_toc ? vm_read32(g_yz_game_toc - 0x72E8u) : 0u;
    const bool arena_readable = addr_readable(command_arena + 0x20u);
    const uint32_t command_head =
        arena_readable ? vm_read32(command_arena + 0x00u) : 0u;
    const uint32_t command_count =
        arena_readable ? vm_read32(command_arena + 0x1Cu) : 0u;

    if (phase == 1u) {
        probe.before_command_head = command_head;
        probe.before_command_count = command_count;
        fprintf(stderr,
                "[a010-house-mesh] SUBMIT site=%08X mesh=%u "
                "slot=%u "
                "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X "
                "arena=%08X head=%08X count=%u\n",
                site, probe.mesh_index, probe.group_slot,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
                (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8],
                command_arena, command_head, command_count);
        fflush(stderr);
        return;
    }

    if (phase == 2u) {
        fprintf(stderr,
                "[a010-house-mesh] RETURN site=%08X mesh=%u "
                "slot=%u "
                "arena=%08X head=%08X->%08X delta=%d count=%u->%u delta=%d\n",
                site, probe.mesh_index, probe.group_slot, command_arena,
                probe.before_command_head, command_head,
                (int32_t)(command_head - probe.before_command_head),
                probe.before_command_count, command_count,
                (int32_t)(command_count - probe.before_command_count));
        fflush(stderr);
    }
}

/* Natural host EOS must enter CRI through the game's movie-owner lifecycle,
 * not through a fabricated zero-byte file read. The mwPly getter ABIs do not
 * all receive the player in r3 (GetAudioPcmData uses a transient stack
 * argument there), so a value sampled from those calls is not authoritative.
 *
 * The active post-title movie is owned by:
 *   *(manager_global + 0x220 + 0x110) -> owner
 *   *(owner + 0xFC)                   -> movie player
 *
 * Ask the owner to stop on guest PPU thread 1, let its normal update advance
 * state 3 -> 4, then run its cleanup and unregister it. This is the handoff
 * required before a simultaneously loaded auth sequence (for example a010)
 * is allowed to update. */
static volatile uint32_t g_yz_mwply_active_handle = 0;
static __declspec(thread) int g_yz_mwply_stop_injecting = 0;

static bool yz_movie_heap_ptr(uint32_t address)
{
    return address >= 0x00010000u && address < 0x80000000u;
}

static uint32_t yz_active_movie_owner(uint32_t* slot_out)
{
    constexpr uint32_t manager_global = 0x014EC864u;
    const uint32_t manager = vm_read32(manager_global);
    if (!yz_movie_heap_ptr(manager))
        return 0;

    const uint32_t slot = manager + 0x220u + 0x110u;
    if (!yz_movie_heap_ptr(slot))
        return 0;
    if (slot_out)
        *slot_out = slot;

    const uint32_t owner = vm_read32(slot);
    return yz_movie_heap_ptr(owner) ? owner : 0;
}

extern "C" int yz_movie_hle_armed(void);

static void yz_mwply_lifecycle_boundary(void* fn, ppu_context* ctx)
{
    /* Route-1 HLE armed: movies run the native lifecycle + host frame leaves;
     * none of this bridge-era machinery (capture, serials, injected Stop,
     * owner completion) may act. */
    if (yz_movie_hle_armed())
        return;

    static void* start_fn = nullptr;
    static void* stop_fn = nullptr;
    static void* stop_wrapper_fn = nullptr;
    static void* owner_complete_fn = nullptr;
    static void* owner_cleanup_fn = nullptr;
    static void* ready_fn = nullptr;
    static void* frame_fn = nullptr;
    static void* audio_fn = nullptr;
    if (!start_fn) start_fn = (void*)yz_lookup_func(0x00F4E720u);
    if (!stop_fn)  stop_fn  = (void*)yz_lookup_func(0x00F4ED44u);
    if (!stop_wrapper_fn) stop_wrapper_fn = (void*)yz_lookup_func(0x00F495DCu);
    if (!owner_complete_fn) owner_complete_fn = (void*)yz_lookup_func(0x00D37A9Cu);
    if (!owner_cleanup_fn) owner_cleanup_fn = (void*)yz_lookup_func(0x00D37980u);
    if (!ready_fn) ready_fn = (void*)yz_lookup_func(0x00F4D0A8u);
    if (!frame_fn) frame_fn = (void*)yz_lookup_func(0x00F4DA90u);
    if (!audio_fn) audio_fn = (void*)yz_lookup_func(0x00F48E48u);

    /* Handle capture feeds the startup Stop-wrapper fallback. The 0x00F48E48
     * probe (logged "GetAudioPcmData", its historical label) must stay in the
     * capture set: measured on the 07-22/07-23 startup boots it is the ONLY
     * probe that fires for host-presented startup movies (StartFname/
     * IsNextFrmReady/GetFrm never hit), and the value it captures drives the
     * wrapper for both startup serials. Removing it (d27c226) silently killed
     * the fallback. Address 0x00F48E48 is a false functions.json boundary
     * mid-body of func_00F48DA0, a SPURS taskset
     * creator -- NOT mwPlyGetAudioPcmData_PS3 as FLAGS.md claims. The captured
     * r3 is an accidental but boot-stable invariant that satisfies the Stop
     * wrapper; retire it when the real mwPly ABI seam lands. */
    if (fn == start_fn || fn == ready_fn || fn == frame_fn || fn == audio_fn) {
        const uint32_t handle = (uint32_t)ctx->gpr[3];
        if (handle && handle != g_yz_mwply_active_handle) {
            g_yz_mwply_active_handle = handle;
            fprintf(stderr, "[movie] captured mwPly handle=%08X via=%s tid=%u\n",
                    handle,
                    fn == start_fn ? "StartFname" :
                    fn == ready_fn ? "IsNextFrmReady" :
                    fn == frame_fn ? "GetFrm" : "GetAudioPcmData",
                    yz_thread_current_id());
            fflush(stderr);
        }
    }
    if (fn == stop_fn && !g_yz_mwply_stop_injecting) {
        fprintf(stderr, "[movie] guest mwPlyStop handle=%08X tid=%u\n",
                (uint32_t)ctx->gpr[3], yz_thread_current_id());
        fflush(stderr);
    }

    const long serial = g_yz_movie_stop_pending_serial;
    if (serial <= 0 || yz_thread_current_id() != 1 ||
        g_yz_mwply_stop_injecting || fn == stop_fn)
        return;

    /* The owner completion/cleanup path is only proven for post-title movies
     * (a020 after New Game). The 2026-07-23 d27 boot showed startup movies
     * ALSO register a manager owner (Sega serial 1, owner state 2) but its
     * completion request never advances to state 4, stalling the transition.
     * Gate on the confirmed title-to-menu phase bit instead of owner!=0. */
    const bool post_title =
        _InterlockedCompareExchange(&g_yz_cri_yield_phase, 0, 0) != 0;

    uint32_t owner_slot = 0;
    const uint32_t owner = yz_active_movie_owner(&owner_slot);
    if (owner && !post_title) {
        static long logged_serial = 0;
        if (logged_serial != serial) {
            logged_serial = serial;
            fprintf(stderr,
                    "[movie] pre-title owner=%08X ignored; using wrapper "
                    "fallback serial=%ld tid=1\n",
                    owner, serial);
            fflush(stderr);
        }
    }
    if (owner && post_title) {
        const uint32_t state = vm_read32(owner + 0x104u);
        const uint32_t player = vm_read32(owner + 0xFCu);

        if (state == 2u) {
            g_yz_mwply_stop_injecting = 1;
            ppu_context owner_ctx = *ctx;
            owner_ctx.gpr[2] = g_yz_game_toc;
            owner_ctx.gpr[3] = owner;
            owner_ctx.lr = 0;
            void (*saved_trampoline)(void*) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            ((yz_ppu_fn)owner_complete_fn)(&owner_ctx);
            yz_drain_trampolines(&owner_ctx);
            g_trampoline_fn = saved_trampoline;
            fprintf(stderr,
                    "[movie] requested owner completion owner=%08X player=%08X "
                    "state=%u serial=%ld tid=1\n",
                    owner, player, state, serial);
            fflush(stderr);
            g_yz_mwply_stop_injecting = 0;
            return;
        }

        /* State 3 is the owner's asynchronous stop/retire phase. Leave it to
         * the ordinary game update. Once it reports state 4, destroy its
         * player and remove the registration that otherwise masks auth. */
        if (state != 4u)
            return;

        g_yz_mwply_stop_injecting = 1;
        ppu_context cleanup_ctx = *ctx;
        cleanup_ctx.gpr[2] = g_yz_game_toc;
        cleanup_ctx.gpr[3] = owner;
        cleanup_ctx.lr = 0;
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        ((yz_ppu_fn)owner_cleanup_fn)(&cleanup_ctx);
        yz_drain_trampolines(&cleanup_ctx);
        g_trampoline_fn = saved_trampoline;
        vm_write32(owner_slot, 0);
        g_yz_movie_stop_pending_serial = 0;
        _InterlockedExchange(&g_yz_movie_stop_applied_serial, serial);
        fprintf(stderr,
                "[movie] cleaned and unregistered owner=%08X player=%08X "
                "serial=%ld tid=1\n",
                owner, player, serial);
        fflush(stderr);
        g_yz_mwply_stop_injecting = 0;
        return;
    }

    /* Startup movie path (pre-title, whether or not a manager owner is
     * registered): the established mwPly Stop-wrapper. */
    const uint32_t handle = g_yz_mwply_active_handle;
    if (!handle)
        return;

    g_yz_mwply_stop_injecting = 1;
    g_yz_movie_stop_pending_serial = 0;
    ppu_context stop_ctx = *ctx;
    stop_ctx.gpr[2] = g_yz_game_toc;
    stop_ctx.gpr[3] = handle;
    stop_ctx.lr = 0;
    void (*saved_trampoline)(void*) = g_trampoline_fn;
    g_trampoline_fn = nullptr;
    ((yz_ppu_fn)stop_wrapper_fn)(&stop_ctx);
    yz_drain_trampolines(&stop_ctx);
    g_trampoline_fn = saved_trampoline;
    _InterlockedExchange(&g_yz_movie_stop_applied_serial, serial);
    fprintf(stderr,
            "[movie] injected game mwPlyStop wrapper handle=%08X serial=%ld tid=1\n",
            handle, serial);
    fflush(stderr);
    g_yz_mwply_stop_injecting = 0;
}

/* Reverse-map a host fn ptr to its guest address (linear scan, cached miss). */
static uint32_t yz_guest_of_host(void* hf)
{
    for (unsigned i = 0; i < g_yz_func_count; i++)
        if ((void*)g_yz_func_table[i].fn == hf) return g_yz_func_table[i].addr;
    return 0;
}

/* TOC repair, called by every trampoline dispatch (inline DRAIN_TRAMPOLINE
 * macro + yz_drain_trampolines). A gcm callback (e.g. the game's flip/vblank
 * handler) can be invoked by its bare code address rather than its OPD, so the
 * OPD's TOC word is never loaded and the callee runs with r2==0 -> it faults on
 * its first TOC-relative access. The game module has ONE TOC, so restoring it
 * for a game-range target (addr < firmware base 0x02000000) is correct. */
/* Trampoline-hop ring (per thread). Records the host fn ptr of every chunk the
 * trampoline driver dispatches. The host stack is FLAT for cross-chunk chains
 * (each chunk returns to the DRAIN_TRAMPOLINE loop, then the next is called),
 * so a normal stack walk can't reconstruct the guest control-flow path. The
 * crash handler reverse-maps this ring to name the exact chunk sequence that
 * led to a fault. TEMP DIAG (#19d r31=0): strip once the cause is pinned. */
extern "C" __declspec(thread) void*    g_yz_tramp_ring[256] = {};
extern "C" __declspec(thread) uint64_t g_yz_tramp_r31[256]  = {};
extern "C" __declspec(thread) uint64_t g_yz_tramp_r1[256]   = {};
extern "C" __declspec(thread) uint64_t g_yz_tramp_lr[256]   = {};   /* caller return addr at the hop */
extern "C" __declspec(thread) unsigned g_yz_tramp_idx      = 0;

/* USLEEP-CALLER CAPTURE (pt26 producer-stop hunt). When g_yz_catch_caller is armed
 * (by flipadv at the stall), the next time t1 dispatches the usleep helper
 * func_00E5DB94 we grab the LIVE host backtrace -> the real guest poll-loop chain
 * (reliable, unlike the flat async stack scan). main.cpp resolves the addrs. */
extern "C" volatile long g_yz_catch_caller = 0;
extern "C" void*         g_yz_caller_bt[32] = {};
extern "C" volatile long g_yz_caller_bt_n  = 0;

/* T1-ONLY non-suspending PC/LR sampler (env YZ_T1SAMPLE, 2026-07-04, the
 * sem-4-rendezvous-then-silent-spin frontier). g_yz_last_targets below is
 * shared by every guest thread, so it's useless once other threads (t7/t10/
 * t13-24) keep calling indirectly while t1 alone goes quiet -- their hits
 * drown t1's. These are written ONLY when yz_thread_current_id()==1 (one
 * branch, no lock, no suspend -- a diagnostic must never perturb the thread
 * it measures) and read by a low-rate timer thread in
 * main.cpp. g_yz_t1_sample_seq increments on every t1 hop, so the printer
 * can tell "moved since last read" from "still parked at the same site". */
extern "C" volatile uint32_t g_yz_t1_last_target = 0;   /* ctr at t1's last bctr(l) */
extern "C" volatile uint32_t g_yz_t1_last_lr     = 0;   /* lr at t1's last trampoline hop */
extern "C" volatile long     g_yz_t1_sample_seq  = 0;
/* s25: t1's last hop target as a raw HOST fn ptr, written UNGATED (one TLS
 * read + store per hop; no guest resolution — that linear scan stays gated
 * behind YZ_T1SAMPLE). Feeds the park-rel fast tier's SPIN witness: a t1
 * orbiting the gcm progress-throttle usleep loop hops forever (seq climbs)
 * but lands on the same target — it makes no flush call in that loop
 * (stopper_drain_re.md Q1), so it is exactly as unable to drain its own
 * journal as a frozen t1. Measured: rides s25ride4-6 ground at 3-5 s/flip
 * because the seq-frozen witness never fired against the spin. */
extern "C" volatile void*    g_yz_t1_last_tf     = 0;

/* s21 HOP CENSUS: size the emission-rework payoff before building it. Every
 * trampoline hop passes this guard; only register-target (bctr-class) calls
 * pass ps3_indirect_call. total - indirect = hops whose target was STATICALLY
 * KNOWN at lift time and trampolined purely for stack discipline -- the
 * fraction the switch/goto + direct-call emission eliminates. Printed once
 * per 10M hops. Counters are per-process, racy-increment tolerable (census). */
extern "C" unsigned long long g_yz_hops_total = 0, g_yz_hops_indirect = 0;

extern "C" void yz_tramp_guard(void* tf, void* ctxv)
{
    ppu_context* ctx0 = (ppu_context*)ctxv;
    yz_mwply_lifecycle_boundary(tf, ctx0);
    yz_a010_auth_probe_poll();
    if ((++g_yz_hops_total & 0x9FFFFFull) == 0) /* ~every 10.5M */
        fprintf(stderr, "[hops] total=%llu indirect=%llu static=%llu (%.1f%% static)\n",
                g_yz_hops_total, g_yz_hops_indirect,
                g_yz_hops_total - g_yz_hops_indirect,
                100.0 * (double)(g_yz_hops_total - g_yz_hops_indirect) / (double)g_yz_hops_total);
    unsigned _ri = g_yz_tramp_idx++ & 255;
    g_yz_tramp_ring[_ri] = tf;
    g_yz_tramp_r31[_ri]  = ctx0->gpr[31];
    g_yz_tramp_r1[_ri]   = ctx0->gpr[1];
    g_yz_tramp_lr[_ri]   = ctx0->lr;     /* = caller's return addr (names the caller) */

    /* YZ_T1SAMPLE (see globals above): covers the same-chunk direct-tail-branch
     * hop (g_trampoline_fn = func_X; return;) which ps3_indirect_call never
     * sees. tf is a host fn ptr here, not a guest addr -- resolve is left to
     * the printer/tools (yz_guest_of_host runs in this TU). */
    /* s21 PERF: this block exists only to feed the YZ_T1SAMPLE diagnostic, but
     * it ran UNGATED -- and yz_guest_of_host is a LINEAR SCAN over all ~57k
     * lifted functions, executed on EVERY t1 trampoline hop. Profiled as the
     * main thread's dominant cost (yz_tramp_guard+0xA5,
     * scratch/hot_threads_id.md). Gate it on the flag it serves. */
    if (yz_thread_current_id() == 1) {
        g_yz_t1_last_tf = tf;            /* ungated spin-witness feed (s25) */
        g_yz_t1_sample_seq++;
        static int t1s = -1; if (t1s < 0) t1s = getenv("YZ_T1SAMPLE") ? 1 : 0;
        if (t1s) {
            uint32_t g = yz_guest_of_host(tf);
            if (g) g_yz_t1_last_target = g;
            g_yz_t1_last_lr = (uint32_t)ctx0->lr;
        }
    }

    /* Exact producer-side stopper-release decisions.  These split functions
     * are the immediate gate, journal fallback, and common continuation of
     * func_00E9BC9C's old-stopper handling.  The recorder is memory-only
     * until a guarded missing-release failure asks it to dump. */
    {
        static int rel_init = 0;
        static void* rel_fn[3];
        if (!rel_init) {
            rel_init = 1;
            rel_fn[0] = (void*)yz_lookup_func(0x00E9BE4Cu);
            rel_fn[1] = (void*)yz_lookup_func(0x00E9BD0Cu);
            rel_fn[2] = (void*)yz_lookup_func(0x00E9BD38u);
        }
        if (g_yz_a010_release_scene_active != 0) {
            static const uint32_t rel_pc[3] = {
                0x00E9BE4Cu, 0x00E9BD0Cu, 0x00E9BD38u
            };
            for (unsigned ri = 0; ri < 3u; ++ri) {
                if (tf == rel_fn[ri] && rel_fn[ri]) {
                    yz_a010_reltrace_ppu(rel_pc[ri], ctx0);
                    break;
                }
            }
        }
    }

    if (g_yz_catch_caller) {
        static void* usleep_fn = nullptr;
        if (!usleep_fn) usleep_fn = (void*)yz_lookup_func(0x00E5DB94u);
        if (tf == usleep_fn) {
            g_yz_caller_bt_n = (long)RtlCaptureStackBackTrace(0, 32, g_yz_caller_bt, nullptr);
            g_yz_catch_caller = 0;
        }
    }

    /* DIAG (2026-06-16, drain hunt, env YZ_DRAIN_TRACE): persistent counters for the
     * gcm release/defer/drain functions, to settle whether the producer EVER reaches
     * the drain (func_00E9B7C0/B8E0 -- applies the op-list tag-0x7F releases) vs only
     * the defer path (func_00E9BDD0). func_00E9BF14 clears S[0x1C]. */
    { static int en = -1; if (en < 0) en = getenv("YZ_DRAIN_TRACE") ? 1 : 0;
      if (en) {
        static void* fn[4]; static long c[4]; static int init = 0;
        if (!init) { init = 1;
            fn[0]=(void*)yz_lookup_func(0x00E9BDD0u);   /* defer (sets S[0x1C]) */
            fn[1]=(void*)yz_lookup_func(0x00E9BF14u);   /* flush (clears S[0x1C]) */
            fn[2]=(void*)yz_lookup_func(0x00E9B7C0u);   /* drain-family flush */
            fn[3]=(void*)yz_lookup_func(0x00E9B8E0u); } /* drain loop */
        for (int i = 0; i < 4; i++) if (tf == fn[i] && fn[i]) {
            static const uint32_t a[4] = {0xE9BDD0u,0xE9BF14u,0xE9B7C0u,0xE9B8E0u};
            if ((c[i]++ & 0x1FF) == 0)
                fprintf(stderr, "[drain] func_%08X hit #%ld tid=%u\n", a[i], c[i], yz_thread_current_id());
        }
      } }

    /* mwPly PROBE, entry-args-only path (env YZ_MWPLY_PROBE, 2026-07-04): this
     * trampoline hop covers BOTH the bctr-indirect case (already fully wrapped
     * in ps3_indirect_call, which returns before setting g_trampoline_fn for
     * these targets, so it does NOT reach here) and the same-chunk direct
     * tail-branch case (e.g. func_00F48E48, reached via
     * `g_trampoline_fn = func_00F48E48; return;` inside another lifted
     * function's fallthrough -- ps3_indirect_call never runs for that path).
     * DRAIN_TRAMPOLINE captures the callee into a local before this guard runs
     * and calls it unconditionally right after, so we cannot wrap the return
     * here without editing generated code -- entry args only. */
    { static int en3 = -1; if (en3 < 0) en3 = getenv("YZ_MWPLY_PROBE") ? 1 : 0;
      if (en3) {
        static void* mwfn[3]; static long c3[3]; static int init3 = 0;
        if (!init3) { init3 = 1;
            mwfn[0] = (void*)yz_lookup_func(0x00F4D0A8u);   /* mwPlyIsNextFrmReady */
            mwfn[1] = (void*)yz_lookup_func(0x00F4DA90u);   /* mwPlyGetFrm */
            mwfn[2] = (void*)yz_lookup_func(0x00F48E48u); } /* mwPlyGetAudioPcmData_PS3 */
        static const char* mwnm[3] = { "mwPlyIsNextFrmReady", "mwPlyGetFrm", "mwPlyGetAudioPcmData_PS3" };
        for (int i = 0; i < 3; i++) if (tf == mwfn[i] && mwfn[i]) {
            long c = ++c3[i];
            if (c <= 40 || (c % 500) == 0)
                fprintf(stderr, "[mwply-tramp] %s (direct-tail-branch hop) tid=%u r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX\n",
                        mwnm[i], yz_thread_current_id(),
                        (unsigned long long)ctx0->gpr[3], (unsigned long long)ctx0->gpr[4],
                        (unsigned long long)ctx0->gpr[5], (unsigned long long)ctx0->gpr[6]);
        }
      } }

    ppu_context* ctx = (ppu_context*)ctxv;
    if (!g_yz_game_toc || ctx->gpr[2] == g_yz_game_toc) return;
    uint32_t g = yz_guest_of_host(tf);
    if (!g || g >= 0x02000000u) return;   /* game module: single TOC */
    static int n = 0;
    if (n < 6) { n++;
        fprintf(stderr, "[toc-fix] r2 0x%08llX -> 0x%08X for func_%08X "
                "(game fn invoked with wrong/no TOC)\n",
                (unsigned long long)ctx->gpr[2], g_yz_game_toc, g);
    }
    ctx->gpr[2] = g_yz_game_toc;
}

extern "C" void yz_drain_trampolines(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        yz_tramp_guard((void*)tf, (void*)ctx);
        tf((void*)ctx);
    }
}

/* Imported firmware functions: fake-key range -> host HLE bridge index. */
static yz_ppu_fn import_bridge_for(uint32_t code)
{
    if (code >= YZ_IMPORT_FAKE_BASE &&
        code <  YZ_IMPORT_FAKE_BASE + g_yz_import_count * 4)
        return g_yz_import_bridges[(code - YZ_IMPORT_FAKE_BASE) / 4];
    if (code == YZ_GCM_CB_FAKE_KEY)   /* runner-owned gcm fifo callback */
        return yz_gcm_fifo_callback;
    return nullptr;
}

/* Ring buffer of recent indirect-call targets, dumped by the crash handler. */
uint32_t g_yz_last_targets[16];
unsigned g_yz_last_idx;

/* Defined in import_overrides.cpp (extern "C"); set by YZ_TASK_TRACE below. */
extern "C" uint32_t g_yz_spurs_taskset;
extern "C" uint32_t g_yz_codec_taskset;   /* pt35: cri_audio codec taskset (wid 3) */

/* Defined in main.cpp with plain C++ linkage (shared by the crash handler and
 * the stall watchdog). Declared here at FILE SCOPE, outside any extern "C"
 * context -- a local `extern` forward-decl nested inside ps3_indirect_call
 * (itself `extern "C"`) gets C linkage in MSVC and fails to link against the
 * C++-mangled definition. */
void yz_dump_guest_state(const ppu_context* gc, const char* tag);

/* Public helper from runtime/syscalls/sys_event.c (declared here rather than
 * pulling in sys_event.h, matching this file's existing forward-decl style
 * for cross-module calls). Used only by the YZ_EVFLAG_FORCE fallback below. */
extern "C" int sys_event_queue_push_by_id(uint32_t queue_id,
                                           uint64_t source, uint64_t data1,
                                           uint64_t data2,  uint64_t data3);

/*
 * Static-stage GMD conversion trace.
 *
 * The healthy capture's first missing environment draw maps byte-for-byte to
 * st_asagao_e_house.gmd.  The raw GSGM archive is still resident and intact in
 * the broken run, so follow it through the separate static GMD loader:
 *
 *   func_E878DC -> func_E72D38 -> func_E7273C
 *       -> indirect allocator (LR E7287C) -> GMD pointer table (r2-7680)
 *
 * At the allocator boundary, E7273C keeps the raw file in r26.  Its caller
 * E72D38 saved the manager's requested table slot in its r31 save area.  This
 * gives us an exact raw -> allocation -> publication chain without changing
 * generated code or guessing from unrelated character-model structures.
 */
struct yz_stage_gmd_trace_record {
    const char* name;
    uint32_t raw;
    uint32_t slot;
    uint32_t requested_bytes;
    uint32_t allocation;
    uint32_t game_toc;
    unsigned allocator_hits;
};

static yz_stage_gmd_trace_record g_yz_stage_gmd_records[] = {
    {"st_asagao_e_house",  0, 0xFFFFFFFFu, 0, 0, 0, 0},
    {"st_asagao_e_tikei",  0, 0xFFFFFFFFu, 0, 0, 0, 0},
    {"st_asagao_e_freeze", 0, 0xFFFFFFFFu, 0, 0, 0, 0},
    {"st_asagao_e_enkei",  0, 0xFFFFFFFFu, 0, 0, 0, 0},
};
extern "C" volatile uint32_t g_yz_stage_house_raw = 0;
extern "C" volatile uint32_t g_yz_stage_house_allocation = 0;
extern "C" volatile uint32_t g_yz_stage_house_bytes = 0;
extern "C" volatile uint32_t g_yz_stage_house_slot = 0xFFFFFFFFu;
extern "C" volatile uint32_t g_yz_stage_house_published = 0;

static bool yz_guest_string_equals(uint32_t address, const char* expected)
{
    if (!addr_readable(address) || !expected)
        return false;
    const char* actual = (const char*)(vm_base + address);
    for (unsigned i = 0; expected[i]; ++i) {
        if (!addr_readable(address + i) || actual[i] != expected[i])
            return false;
    }
    return true;
}

static yz_stage_gmd_trace_record*
yz_stage_gmd_allocator_enter(ppu_context* ctx, uint32_t target)
{
    const uint32_t callsite = (uint32_t)ctx->lr;
    if (!YZ_DIAG_ENABLED("YZ_STAGE_GMD_TRACE") ||
        (callsite != 0x00E7287Cu &&
         callsite != 0x00E72AC4u))
        return nullptr;

    const uint32_t raw = (uint32_t)ctx->gpr[26];
    if (!addr_readable(raw + 0x12u) ||
        vm_read32(raw) != 0x4753474Du)
        return nullptr;

    yz_stage_gmd_trace_record* record = nullptr;
    for (auto& candidate : g_yz_stage_gmd_records) {
        const bool external_name =
            yz_guest_string_equals(raw + 0x12u, candidate.name);
        const bool parsed_house_name =
            &candidate == &g_yz_stage_gmd_records[0] &&
            yz_guest_string_equals(raw + 0x12u,
                                   "st_asagao_seaser_scx060");
        if (external_name || parsed_house_name) {
            if (&candidate == &g_yz_stage_gmd_records[0] &&
                candidate.allocation != 0u &&
                candidate.requested_bytes >=
                    (uint32_t)ctx->gpr[4]) {
                continue;
            }
            record = &candidate;
            break;
        }
    }
    if (!record)
        return nullptr;

    const uint32_t parser_sp = (uint32_t)ctx->gpr[1];
    const uint32_t wrapper_sp =
        addr_readable(parser_sp + 8u)
            ? (uint32_t)vm_read64(parser_sp) : 0u;
    const uint32_t slot =
        addr_readable(wrapper_sp + 0x1A8u)
            ? (uint32_t)vm_read64(wrapper_sp + 0x1A8u)
            : 0xFFFFFFFFu;
    const uint32_t saved_toc =
        addr_readable(parser_sp + 0x30u)
            ? (uint32_t)vm_read64(parser_sp + 0x28u) : 0u;

    record->raw = raw;
    record->slot = slot;
    record->requested_bytes = (uint32_t)ctx->gpr[4];
    record->game_toc = saved_toc;
    if (record == &g_yz_stage_gmd_records[0]) {
        _InterlockedExchange(
            (volatile long*)&g_yz_stage_house_raw, (long)raw);
        _InterlockedExchange(
            (volatile long*)&g_yz_stage_house_bytes,
            (long)record->requested_bytes);
        _InterlockedExchange(
            (volatile long*)&g_yz_stage_house_slot, (long)slot);
    }
    ++record->allocator_hits;
    fprintf(stderr,
            "[stage-gmd] ALLOC-ENTER name=%s raw=%08X slot=%08X "
            "bytes=%08X allocator=%08X allocatorToc=%08X gameToc=%08X "
            "parserSp=%08X wrapperSp=%08X callsite=%08X hit=%u\n",
            record->name, raw, slot, record->requested_bytes, target,
            (uint32_t)ctx->gpr[2], saved_toc, parser_sp, wrapper_sp,
            callsite, record->allocator_hits);
    fflush(stderr);
    return record;
}

static void yz_stage_gmd_allocator_return(yz_stage_gmd_trace_record* record,
                                          ppu_context* ctx)
{
    if (!record)
        return;
    record->allocation = (uint32_t)ctx->gpr[3];
    if (record == &g_yz_stage_gmd_records[0])
        _InterlockedExchange(
            (volatile long*)&g_yz_stage_house_allocation,
            (long)record->allocation);
    fprintf(stderr,
            "[stage-gmd] ALLOC-RETURN name=%s raw=%08X slot=%08X "
            "bytes=%08X allocation=%08X\n",
            record->name, record->raw, record->slot,
            record->requested_bytes, record->allocation);
    fflush(stderr);
}

static void yz_stage_gmd_publication_poll(ppu_context* ctx,
                                          uint32_t active_a010_root)
{
    static bool printed = false;
    if (printed || !YZ_DIAG_ENABLED("YZ_STAGE_GMD_TRACE") ||
        active_a010_root == 0u)
        return;
    if (g_yz_stage_gmd_records[0].raw == 0u)
        return;

    uint32_t toc = g_yz_game_toc;
    for (const auto& record : g_yz_stage_gmd_records) {
        if (record.game_toc) {
            toc = record.game_toc;
            break;
        }
    }
    if (!addr_readable(toc - 0x7680u))
        return;
    const uint32_t table = vm_read32(toc - 0x7680u);
    if (!addr_readable(table + 0x3FFCu))
        return;

    printed = true;
    unsigned populated = 0;
    for (unsigned slot = 0; slot < 4096u; ++slot) {
        if (vm_read32(table + slot * 4u))
            ++populated;
    }
    fprintf(stderr,
            "[stage-gmd] PUBLICATION-SNAPSHOT root=%08X toc=%08X "
            "table=%08X populated=%u trigger=%08X lr=%08X\n",
            active_a010_root, toc, table, populated,
            (uint32_t)ctx->ctr, (uint32_t)ctx->lr);

    for (const auto& record : g_yz_stage_gmd_records) {
        const uint32_t published =
            record.slot < 4096u
                ? vm_read32(table + record.slot * 4u) : 0u;
        if (&record == &g_yz_stage_gmd_records[0])
            _InterlockedExchange(
                (volatile long*)&g_yz_stage_house_published,
                (long)published);
        unsigned raw_refs = 0;
        unsigned name_copies = 0;
        const uint32_t scan_bytes =
            record.allocation && record.requested_bytes
                ? (record.requested_bytes < 0x02000000u
                       ? record.requested_bytes : 0x02000000u)
                : 0u;
        if (addr_readable(record.allocation) && scan_bytes >= 4u) {
            const size_t name_len = strlen(record.name);
            for (uint32_t off = 0;
                 off + 4u <= scan_bytes &&
                 addr_readable(record.allocation + off + 4u);
                 off += 4u) {
                if (vm_read32(record.allocation + off) == record.raw)
                    ++raw_refs;
            }
            for (uint32_t off = 0;
                 off + name_len <= scan_bytes &&
                 addr_readable(record.allocation + off +
                               (uint32_t)name_len);
                 ++off) {
                if (memcmp(vm_base + record.allocation + off,
                           record.name, name_len) == 0)
                    ++name_copies;
            }
        }
        fprintf(stderr,
                "[stage-gmd] PUBLICATION name=%s raw=%08X slot=%08X "
                "bytes=%08X allocation=%08X published=%08X "
                "rawRefs=%u nameCopies=%u\n",
                record.name, record.raw, record.slot,
                record.requested_bytes, record.allocation, published,
                raw_refs, name_copies);
        if (published && addr_readable(published + 0x100u)) {
            for (uint32_t off = 0; off < 0x100u; off += 0x20u) {
                fprintf(stderr,
                        "[stage-gmd] OBJECT name=%s +%03X:"
                        " %08X %08X %08X %08X %08X %08X %08X %08X\n",
                        record.name, off,
                        vm_read32(published + off + 0x00u),
                        vm_read32(published + off + 0x04u),
                        vm_read32(published + off + 0x08u),
                        vm_read32(published + off + 0x0Cu),
                        vm_read32(published + off + 0x10u),
                        vm_read32(published + off + 0x14u),
                        vm_read32(published + off + 0x18u),
                        vm_read32(published + off + 0x1Cu));
            }
        }
    }
    fflush(stderr);
    yz_stage_house_external_snapshot();
}

static bool yz_stage_render_method(uint32_t target)
{
    return target == 0x00FE4508u ||
           target == 0x00FE5DB8u ||
           target == 0x00FE70A0u ||
           target == 0x00FE8CA0u ||
           target == 0x00FEB178u ||
           target == 0x00FEBC00u;
}

static bool yz_stage_render_instance(uint32_t object)
{
    return object >= 0x40000000u &&
           object <= 0x7FFFFE80u &&
           vm_read32(object) == 0x011D6948u;
}

#if defined(YZ_PPU_SAMPLE)
struct yz_mt_entry_aggregate {
    uint32_t target;
    uint32_t calls;
    uint64_t elapsed_us;
};
static thread_local bool g_yz_mt_outer_active;
static thread_local uint32_t g_yz_mt_entry_calls;
static thread_local uint64_t g_yz_mt_entry_elapsed_us;
static thread_local yz_mt_entry_aggregate g_yz_mt_entries[64];
static thread_local unsigned g_yz_mt_entry_count;
#endif

extern "C" void ps3_indirect_call(ppu_context* ctx)
{
    g_yz_hops_indirect++;                     /* s21 hop census (see yz_tramp_guard) */
    uint32_t target = (uint32_t)ctx->ctr;
    const uint32_t dispatch_object = (uint32_t)ctx->gpr[3];
    g_yz_last_targets[g_yz_last_idx++ & 15] = target;
    const uint32_t active_a010_root = yz_active_a010_root();
    yz_a010_direct_probe_poll(target);
    yz_stage_gmd_trace_record* const stage_gmd_allocator =
        yz_stage_gmd_allocator_enter(ctx, target);
    yz_stage_gmd_publication_poll(ctx, active_a010_root);
    if (YZ_DIAG_ENABLED("YZ_STAGE_INSTANCE_TRACE") &&
        active_a010_root != 0u) {
        constexpr uint32_t stage_instance_vtable = 0x011D6948u;
        uint32_t instance = 0u;
        unsigned this_reg = 0u;
        const uint32_t r3 = (uint32_t)ctx->gpr[3];
        const uint32_t r4 = (uint32_t)ctx->gpr[4];
        /* Static-stage instances are sys_memory objects in the committed
         * 0x40000000..0x7fffffff arena.  addr_readable() intentionally covers
         * the larger guest reservation, including uncommitted holes; probing
         * one such 0x8000xxxx argument here caused the Run22/23 diagnostic AV. */
        const auto stage_instance_candidate = [](uint32_t address) {
            return address >= 0x40000000u &&
                   address <= 0x7FFFFE80u;
        };
        if (stage_instance_candidate(r3) &&
            vm_read32(r3) == stage_instance_vtable) {
            instance = r3;
            this_reg = 3u;
        } else if (stage_instance_candidate(r4) &&
                   vm_read32(r4) == stage_instance_vtable) {
            instance = r4;
            this_reg = 4u;
        }
        if (instance) {
            struct yz_stage_instance_call {
                uint32_t target;
                unsigned hits;
            };
            static yz_stage_instance_call calls[32] = {};
            static unsigned call_count = 0;
            unsigned slot = call_count;
            for (unsigned i = 0; i < call_count; ++i) {
                if (calls[i].target == target) {
                    slot = i;
                    break;
                }
            }
            if (slot == call_count && call_count < 32u)
                calls[call_count++] = {target, 0u};
            if (slot < 32u) {
                yz_stage_instance_call& call = calls[slot];
                ++call.hits;
                if (call.hits <= 16u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[stage-instance] target=%08X lr=%08X "
                            "hits=%u this=r%u:%08X "
                            "gmd=%08X mask=%08X f0C=%08X f10=%08X "
                            "f14=%08X f18=%08X f1C=%08X f20=%08X "
                            "r5=%08X r6=%08X r7=%08X r8=%08X\n",
                            target, (uint32_t)ctx->lr, call.hits,
                            this_reg, instance,
                            vm_read32(instance + 0x04u),
                            vm_read32(instance + 0x08u),
                            vm_read32(instance + 0x0Cu),
                            vm_read32(instance + 0x10u),
                            vm_read32(instance + 0x14u),
                            vm_read32(instance + 0x18u),
                            vm_read32(instance + 0x1Cu),
                            vm_read32(instance + 0x20u),
                            (uint32_t)ctx->gpr[5],
                            (uint32_t)ctx->gpr[6],
                            (uint32_t)ctx->gpr[7],
                            (uint32_t)ctx->gpr[8]);
                    fflush(stderr);
                }
            }
        }
    }
    /*
     * a010's update finishes with the character enabled, but another phase
     * clears object+0x28 before func_00015480 consumes it. Restore the
     * authored value at that final render gate; the normal update and render
     * implementations otherwise run unchanged.
     */
    if (g_yz_runtime_config.a010_render_enable &&
        target == 0x00015480u &&
        addr_readable(dispatch_object) &&
        vm_read32(dispatch_object) == 0x011D3F78u &&
        active_a010_root != 0u) {
        uint32_t timeline_bits = vm_read32(active_a010_root + 0x210u);
        float timeline = 0.0f;
        memcpy(&timeline, &timeline_bits, sizeof(timeline));
        const bool authored_visible =
            yz_a010_authored_visible(dispatch_object, timeline) ||
            (timeline >= 0.0f && timeline < 1068.0f);
        if (authored_visible &&
            addr_readable(dispatch_object + 0x28u)) {
            const uint32_t prior_visible =
                vm_read32(dispatch_object + 0x28u);
            vm_write32(dispatch_object + 0x28u, 1u);
            static unsigned long repairs;
            repairs++;
            if (repairs <= 32u ||
                (repairs & (repairs - 1u)) == 0u) {
                fprintf(stderr,
                        "[a010-render-enable-repair] n=%lu object=%08X "
                        "time=%.3f prior=%u visible=1\n",
                        repairs, dispatch_object,
                        (double)timeline, prior_visible);
                fflush(stderr);
            }
        }
    }
    /*
     * The orphanage characters can finish their first model initialization
     * before the 1 MiB section arena at 0x01364DF4 is installed.  That leaves
     * both model+0x218 and item+0x2C null, but the caller clears its bit-27
     * "initialize model" flag anyway.  Once the arena is live, restore only
     * that retry flag.  Render pass 7 then calls the game's own func_00094288
     * -> func_00092CE0 path with the original object+0xA64 arguments, which
     * allocates and populates the section buffers.
     */
    if (g_yz_runtime_config.a010_model_retry &&
        target == 0x00015480u &&
        addr_readable(dispatch_object + 0x48u) &&
        vm_read32(dispatch_object) == 0x011D3F78u &&
        active_a010_root != 0u) {
        const uint32_t model = vm_read32(dispatch_object + 0x2Cu);
        const uint32_t resource =
            addr_readable(model + 0x20Cu)
                ? vm_read32(model + 0x20Cu) : 0u;
        const uint32_t resource_data =
            addr_readable(resource + 4u)
                ? vm_read32(resource + 4u) : 0u;
        const uint32_t sentinel = model + 0x248u;
        const uint32_t list_head =
            addr_readable(sentinel) ? vm_read32(sentinel) : 0u;
        const uint32_t item =
            list_head != sentinel && addr_readable(list_head + 8u)
                ? vm_read32(list_head + 8u) : 0u;
        const uint32_t item_part =
            addr_readable(item + 4u) ? vm_read32(item + 4u) : 0u;
        const uint32_t item_mesh =
            addr_readable(item_part + 0x70u)
                ? vm_read32(item_part + 0x70u) : 0u;
        const uint32_t item_mesh_data =
            addr_readable(item_mesh + 4u)
                ? vm_read32(item_mesh + 4u) : 0u;
        const unsigned resource_sections =
            addr_readable(resource_data + 0x32u)
                ? (unsigned)vm_read16(resource_data + 0x32u) : 0u;
        const unsigned item_sections =
            addr_readable(item_mesh_data + 0x32u)
                ? (unsigned)vm_read16(item_mesh_data + 0x32u) : 0u;
        const uint32_t model_default =
            addr_readable(model + 0x218u)
                ? vm_read32(model + 0x218u) : 0u;
        const uint32_t item_default =
            addr_readable(item + 0x2Cu)
                ? vm_read32(item + 0x2Cu) : 0u;
        const uint32_t arena = vm_read32(0x01364DF4u);
        const uint32_t arena_cur =
            addr_readable(arena + 8u) ? vm_read32(arena + 8u) : 0u;
        const uint32_t arena_alt =
            addr_readable(arena + 0xCu) ? vm_read32(arena + 0xCu) : 0u;
        const uint32_t arena_end =
            addr_readable(arena + 0x10u) ? vm_read32(arena + 0x10u) : 0u;
        const uint64_t bytes = (uint64_t)resource_sections * 0x30u;
        const bool arena_ready =
            arena != 0u && arena_end != 0u &&
            ((uint64_t)arena_cur + bytes < arena_end ||
             (uint64_t)arena_alt + bytes < arena_end);
        if (model_default == 0u && item_default == 0u &&
            resource_sections != 0u &&
            resource_sections == item_sections &&
            arena_ready) {
            const uint32_t prior_flags =
                vm_read32(dispatch_object + 0x48u);
            constexpr uint32_t initialize_model_bit = 0x08000000u;
            if ((prior_flags & initialize_model_bit) == 0u) {
                vm_write32(dispatch_object + 0x48u,
                           prior_flags | initialize_model_bit);
                static unsigned long retries;
                retries++;
                if (retries <= 32u ||
                    (retries & (retries - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-model-retry] n=%lu object=%08X "
                            "model=%08X sections=%u arena=%08X "
                            "cur=%08X alt=%08X end=%08X flags=%08X->%08X\n",
                            retries, dispatch_object, model,
                            resource_sections, arena, arena_cur, arena_alt,
                            arena_end, prior_flags,
                            prior_flags | initialize_model_bit);
                    fflush(stderr);
                }
            }
        }
    }
    /*
     * Pass 13 installs the transient model palette and the character renderer
     * walks passes in descending order. Publish any missing authored palette
     * immediately before pass 0 consumes it.
     */
    if (g_yz_runtime_config.a010_palette_repair &&
        target == 0x00015480u &&
        (uint32_t)ctx->gpr[4] == 0u &&
        active_a010_root != 0u) {
        yz_a010_publish_character_palette(dispatch_object,
                                          active_a010_root);
    }
    /*
     * a010 arrives with the final camera track (1116..1227) installed in the
     * active node while the opening track (0..1068) is still in the pending
     * node.  The scene and character clocks correctly begin at zero, so the
     * active evaluator rejects every opening frame.  Preserve the list
     * topology and repair only the active node's track pointer when the
     * pending track owns the current scene frame.
     */
    if (g_yz_runtime_config.a010_camera_handoff &&
        target == 0x00011DF0u &&
        active_a010_root != 0u &&
        addr_readable(dispatch_object + 0x20u) &&
        vm_read32(dispatch_object) == 0x011D3EA0u) {
        const uint32_t pending_node = vm_read32(dispatch_object + 0x1Cu);
        const uint32_t active_node = vm_read32(dispatch_object + 0x20u);
        if (addr_readable(pending_node) && addr_readable(active_node)) {
            const uint32_t pending_track = vm_read32(pending_node);
            const uint32_t active_track = vm_read32(active_node);
            if (addr_readable(pending_track + 0x1Cu) &&
                addr_readable(active_track + 0x1Cu)) {
                float pending_start = 0.0f;
                float pending_end = 0.0f;
                float active_start = 0.0f;
                float active_end = 0.0f;
                uint32_t bits = vm_read32(pending_track + 0x18u);
                memcpy(&pending_start, &bits, sizeof(pending_start));
                bits = vm_read32(pending_track + 0x1Cu);
                memcpy(&pending_end, &bits, sizeof(pending_end));
                bits = vm_read32(active_track + 0x18u);
                memcpy(&active_start, &bits, sizeof(active_start));
                bits = vm_read32(active_track + 0x1Cu);
                memcpy(&active_end, &bits, sizeof(active_end));
                const float time = (float)ctx->fpr[1];
                const bool pending_owns =
                    time >= pending_start && time < pending_end;
                const bool active_owns =
                    time >= active_start && time < active_end;
                if (pending_owns && !active_owns) {
                    vm_write32(active_node, pending_track);
                    static unsigned long repairs;
                    repairs++;
                    if (repairs <= 16u ||
                        (repairs & (repairs - 1u)) == 0u) {
                        fprintf(stderr,
                                "[a010-camera-handoff-repair] n=%lu "
                                "object=%08X node=%08X "
                                "track=%08X->%08X time=%.3f "
                                "pending=%.3f..%.3f "
                                "active=%.3f..%.3f\n",
                                repairs, dispatch_object, active_node,
                                active_track, pending_track, (double)time,
                                (double)pending_start, (double)pending_end,
                                (double)active_start, (double)active_end);
                        fflush(stderr);
                    }
                }
            }
        }
    }
    yz_auto_new_game_cached_input();
    /*
     * Exact a010 activation input at the character-update boundary.  Earlier
     * diagnostics reported the outer callback's normal zero return as though
     * it were the linked-track evaluator's result.  Instead, snapshot the
     * actual time and every authored range entering func_00014848 for a short
     * window after the post-load rewind.  The object+0x28 write-watch supplies
     * the corresponding producer/writer side without editing generated code.
     */
    if (g_yz_a010_root_active != 0 && target == 0x00014848u) {
        static float prior_time = 0.0f;
        static unsigned rewind_window = 0;
        static unsigned long snapshot_count = 0;
        static uint32_t initial_objects[16] = {};
        static unsigned initial_object_count = 0;
        const float time = (float)ctx->fpr[1];
        const uint32_t object = (uint32_t)ctx->gpr[3];
        const bool a010_trace = YZ_DIAG_ENABLED("YZ_A010_TRACE");
        if (g_yz_a010_animation_ready == 0 &&
            yz_a010_character_animation_ready(object) &&
            _InterlockedCompareExchange(
                &g_yz_a010_animation_ready, 1, 0) == 0) {
            if (a010_trace) {
                const uint32_t model = vm_read32(object + 0x2Cu);
                const uint32_t matrices = vm_read32(model + 0x214u);
                const uint32_t palette = vm_read32(model + 0x218u);
                uint32_t matrix_y_bits = vm_read32(matrices + 0x34u);
                uint32_t palette_y_bits = vm_read32(palette + 0x1Cu);
                float matrix_y = 0.0f;
                float palette_y = 0.0f;
                memcpy(&matrix_y, &matrix_y_bits, sizeof(matrix_y));
                memcpy(&palette_y, &palette_y_bits, sizeof(palette_y));
                fprintf(stderr,
                        "[a010-animation-ready] object=%08X model=%08X "
                        "matrices=%08X palette=%08X y=%.3f/%.3f time=%.3f\n",
                        object, model, matrices, palette,
                        (double)matrix_y, (double)palette_y, (double)time);
                fflush(stderr);
            }
        }
        if (a010_trace) {
        bool first_for_object = false;
        unsigned object_slot = 0;
        for (; object_slot < initial_object_count; object_slot++) {
            if (initial_objects[object_slot] == object)
                break;
        }
        if (object_slot == initial_object_count &&
            initial_object_count < 16u) {
            initial_objects[initial_object_count++] = object;
            first_for_object = true;
        }
        if (first_for_object) {
            uint32_t node = addr_readable(object + 0x1Cu)
                          ? vm_read32(object + 0x1Cu) : 0u;
            fprintf(stderr,
                    "[a010-character-ranges] object=%08X time=%.3f "
                    "head=%08X direct20=%08X\n",
                    object, (double)time, node,
                    addr_readable(object + 0x20u)
                        ? vm_read32(object + 0x20u) : 0u);
            for (unsigned i = 0; i < 32u &&
                 addr_readable(node + 0x20u); i++) {
                const uint32_t track = vm_read32(node + 0x4u);
                uint32_t start_bits = 0;
                uint32_t end_bits = 0;
                float start = 0.0f;
                float end = 0.0f;
                if (addr_readable(track + 0x1Cu)) {
                    start_bits = vm_read32(track + 0x18u);
                    end_bits = vm_read32(track + 0x1Cu);
                    memcpy(&start, &start_bits, sizeof(start));
                    memcpy(&end, &end_bits, sizeof(end));
                }
                fprintf(stderr,
                        "[a010-character-range] object=%08X i=%u "
                        "node=%08X type=%08X track=%08X "
                        "range=%.3f..%.3f raw=%08X/%08X next=%08X\n",
                        object, i, node, vm_read32(node), track,
                        (double)start, (double)end,
                        start_bits, end_bits, vm_read32(node + 0x20u));
                node = vm_read32(node + 0x20u);
            }
            fflush(stderr);
        }
        if (time + 100.0f < prior_time)
            rewind_window = 256;
        prior_time = time;
        if (rewind_window != 0) {
            rewind_window--;
            snapshot_count++;
            uint32_t time_bits = 0;
            memcpy(&time_bits, &time, sizeof(time_bits));
            uint32_t node = addr_readable(object + 0x1Cu)
                          ? vm_read32(object + 0x1Cu) : 0u;
            fprintf(stderr,
                    "[a010-activation-input] n=%lu object=%08X "
                    "time=%08X visible=%08X head=%08X\n",
                    snapshot_count, object, time_bits,
                    addr_readable(object + 0x28u)
                        ? vm_read32(object + 0x28u) : 0u,
                    node);
            for (unsigned i = 0; i < 8u && addr_readable(node + 0x20u); i++) {
                const uint32_t track = vm_read32(node + 0x4u);
                fprintf(stderr,
                        "[a010-activation-node] n=%lu i=%u node=%08X "
                        "state=%08X track=%08X range=%08X/%08X next=%08X\n",
                        snapshot_count, i, node, vm_read32(node), track,
                        addr_readable(track + 0x1Cu)
                            ? vm_read32(track + 0x18u) : 0u,
                        addr_readable(track + 0x1Cu)
                            ? vm_read32(track + 0x1Cu) : 0u,
                        vm_read32(node + 0x20u));
                node = vm_read32(node + 0x20u);
            }
            fflush(stderr);
        }
        }
    }
    /*
     * AUTH character rendering is a separate virtual phase from its timeline
     * update.  The a010 objects use slot 7 (func_00014848) for update and slot
     * 9 (func_00015480) for render submission.  Trace both the engine wrappers
     * that should enter that render phase and the recursive slot-9 walker.
     * This identifies whether the scene is absent from the render registry or
     * whether an individual character rejects submission.
     */
    if (g_yz_a010_root_active != 0 &&
        YZ_DIAG_ENABLED("YZ_A010_TRACE")) {
        const uint32_t lr = (uint32_t)ctx->lr;
        const bool render_wrapper =
            target == 0x00066FE0u || target == 0x0006714Cu ||
            target == 0x000672C0u || target == 0x00067434u ||
            target == 0x000675A8u || target == 0x00067714u ||
            target == 0x00067880u || target == 0x000679ECu ||
            target == 0x00067B64u || target == 0x00067E68u ||
            target == 0x00067FF8u;
        const bool auth_render_slot =
            target == 0x00015480u ||
            lr == 0x00C961C0u || lr == 0x00C96204u;
        if (render_wrapper || auth_render_slot) {
            struct render_call {
                uint32_t lr;
                uint32_t target;
                unsigned hits;
            };
            static render_call seen[64] = {};
            static unsigned seen_count;
            unsigned slot = seen_count;
            for (unsigned i = 0; i < seen_count; ++i) {
                if (seen[i].lr == lr && seen[i].target == target) {
                    slot = i;
                    break;
                }
            }
            if (slot == seen_count && seen_count < 64u)
                seen[seen_count++] = {lr, target, 0u};
            if (slot < 64u) {
                render_call& call = seen[slot];
                ++call.hits;
                if (call.hits <= 8u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    const uint32_t object = (uint32_t)ctx->gpr[3];
                    fprintf(stderr,
                            "[a010-render-phase] lr=%08X target=%08X "
                            "hits=%u object=%08X r4=%08X r5=%08X "
                            "object8=%08X object14=%08X "
                            "f28=%08X f2C=%08X f30=%08X f48=%08X "
                            "fA94=%08X fA9C=%08X\n",
                            lr, target, call.hits, object,
                            (uint32_t)ctx->gpr[4],
                            (uint32_t)ctx->gpr[5],
                            addr_readable(object + 8u)
                                ? vm_read32(object + 8u) : 0u,
                            addr_readable(object + 0x14u)
                                ? vm_read32(object + 0x14u) : 0u,
                            addr_readable(object + 0x28u)
                                ? vm_read32(object + 0x28u) : 0u,
                            addr_readable(object + 0x2Cu)
                                ? vm_read32(object + 0x2Cu) : 0u,
                            addr_readable(object + 0x30u)
                                ? vm_read32(object + 0x30u) : 0u,
                            addr_readable(object + 0x48u)
                                ? vm_read32(object + 0x48u) : 0u,
                            addr_readable(object + 0xA94u)
                                ? vm_read32(object + 0xA94u) : 0u,
                            addr_readable(object + 0xA9Cu)
                                ? vm_read32(object + 0xA9Cu) : 0u);
                    if (target == 0x00015480u) {
                        const uint32_t model =
                            addr_readable(object + 0x2Cu)
                                ? vm_read32(object + 0x2Cu) : 0u;
                        const uint32_t resource =
                            addr_readable(model + 0x20Cu)
                                ? vm_read32(model + 0x20Cu) : 0u;
                        const uint32_t resource_data =
                            addr_readable(resource + 4u)
                                ? vm_read32(resource + 4u) : 0u;
                        const uint32_t sentinel = model + 0x248u;
                        const uint32_t list_head =
                            addr_readable(sentinel)
                                ? vm_read32(sentinel) : 0u;
                        const uint32_t item =
                            list_head != sentinel &&
                            addr_readable(list_head + 8u)
                                ? vm_read32(list_head + 8u) : 0u;
                        const uint32_t item_part =
                            addr_readable(item + 4u)
                                ? vm_read32(item + 4u) : 0u;
                        const uint32_t item_mesh =
                            addr_readable(item_part + 0x70u)
                                ? vm_read32(item_part + 0x70u) : 0u;
                        const uint32_t item_mesh_data =
                            addr_readable(item_mesh + 4u)
                                ? vm_read32(item_mesh + 4u) : 0u;
                        const uint32_t section_arena =
                            vm_read32(0x01364DF4u);
                        fprintf(stderr,
                                "[a010-model-state] object=%08X pass=%08X "
                                "model=%08X flags10=%08X maskC8=%08X "
                                "resource=%08X resourceData=%08X "
                                "resourceSections=%u "
                                "list=%08X sentinel=%08X item=%08X "
                                "itemMask=%08X itemPart=%08X "
                                "modelDefault=%08X itemDefault=%08X "
                                "itemMesh=%08X itemMeshData=%08X "
                                "itemSections=%u arena=%08X "
                                "arenaCur=%08X arenaAlt=%08X arenaEnd=%08X\n",
                                object, (uint32_t)ctx->gpr[4], model,
                                addr_readable(model + 0x10u)
                                    ? vm_read32(model + 0x10u) : 0u,
                                addr_readable(model + 0xC8u)
                                    ? vm_read32(model + 0xC8u) : 0u,
                                resource, resource_data,
                                addr_readable(resource_data + 0x32u)
                                    ? (unsigned)vm_read16(
                                          resource_data + 0x32u) : 0u,
                                list_head, sentinel, item,
                                addr_readable(item + 0x24u)
                                    ? vm_read32(item + 0x24u) : 0u,
                                item_part,
                                addr_readable(model + 0x218u)
                                    ? vm_read32(model + 0x218u) : 0u,
                                addr_readable(item + 0x2Cu)
                                    ? vm_read32(item + 0x2Cu) : 0u,
                                item_mesh, item_mesh_data,
                                addr_readable(item_mesh_data + 0x32u)
                                    ? (unsigned)vm_read16(
                                          item_mesh_data + 0x32u) : 0u,
                                section_arena,
                                addr_readable(section_arena + 8u)
                                    ? vm_read32(section_arena + 8u) : 0u,
                                addr_readable(section_arena + 0xCu)
                                    ? vm_read32(section_arena + 0xCu) : 0u,
                                addr_readable(section_arena + 0x10u)
                                    ? vm_read32(section_arena + 0x10u) : 0u);
                    }
                    fflush(stderr);
                }
            }
        }
        /*
         * Character rendering fans out from func_00015480 into pass-specific
         * model routines.  Capture the values used by their first visibility
         * comparison and the func_00094DA0 submission frontier.  Keeping this
         * in the indirect dispatcher avoids changing generated recompilation
         * output while still distinguishing an outer-object rejection from a
         * fade/visibility rejection inside the selected pass.
         */
        const bool character_model_gate =
            target == 0x00095580u || target == 0x00095E40u ||
            target == 0x000963C0u || target == 0x000970C8u ||
            target == 0x00097268u || target == 0x00097D00u ||
            target == 0x000984E8u || target == 0x00098AE0u ||
            target == 0x00094DA0u;
        if (character_model_gate) {
            struct model_gate_call {
                uint32_t lr;
                uint32_t target;
                uint32_t object;
                unsigned hits;
            };
            static model_gate_call seen[128] = {};
            static unsigned seen_count;
            const uint32_t object = (uint32_t)ctx->gpr[3];
            unsigned slot = seen_count;
            for (unsigned i = 0; i < seen_count; ++i) {
                if (seen[i].lr == lr && seen[i].target == target &&
                    seen[i].object == object) {
                    slot = i;
                    break;
                }
            }
            if (slot == seen_count && seen_count < 128u)
                seen[seen_count++] = {lr, target, object, 0u};
            if (slot < 128u) {
                model_gate_call& call = seen[slot];
                ++call.hits;
                if (call.hits <= 4u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-model-gate] lr=%08X target=%08X "
                            "hits=%u object=%08X fpr1=%.9g "
                            "f20C=%08X f28=%08X f474=%08X f4A4=%08X "
                            "c970C0=%08X\n",
                            lr, target, call.hits, object, ctx->fpr[1],
                            addr_readable(object + 0x20Cu)
                                ? vm_read32(object + 0x20Cu) : 0u,
                            addr_readable(object + 0x28u)
                                ? vm_read32(object + 0x28u) : 0u,
                            addr_readable(object + 0x474u)
                                ? vm_read32(object + 0x474u) : 0u,
                            addr_readable(object + 0x4A4u)
                                ? vm_read32(object + 0x4A4u) : 0u,
                            vm_read32(0x000970C0u));
                    fflush(stderr);
                }
            }
        }
    }
    /*
     * The async file-loader completion wrapper stores the returned blob at
     * object+0x120, then invokes the object's parser at LR 0x0033DE9C.  CMT
     * camera blobs are the narrowest way to identify the parser responsible
     * for turning a valid archive member into the live camera track.
     */
    if (YZ_DIAG_ENABLED("YZ_A010_TRACE") &&
        g_yz_a010_root_active != 0 &&
        (uint32_t)ctx->lr == 0x0033DE9Cu) {
        const uint32_t object = (uint32_t)ctx->gpr[3];
        const uint32_t size =
            addr_readable(object + 0x120u)
                ? vm_read32(object + 0x120u) : 0u;
        const bool is_cmt_size =
            size == 0x1A10u || size == 0x1DF0u || size == 0x2490u ||
            size == 0x2A10u || size == 0x1430u || size == 0x3410u ||
            size == 0x2770u || size == 0x1B90u || size == 0x2A50u;
        if (is_cmt_size) {
            static unsigned cmt_loads;
            cmt_loads++;
            fprintf(stderr,
                    "[a010-cmt-load] n=%u target=%08X object=%08X "
                    "vtable=%08X size=%08X status=%u/%u "
                    "fields124=%08X 128=%08X\n",
                    cmt_loads, target, object,
                    addr_readable(object) ? vm_read32(object) : 0u,
                    size,
                    addr_readable(object + 0x8u)
                        ? (unsigned)vm_read8(object + 0x8u) : 0u,
                    addr_readable(object + 0xAu)
                        ? (unsigned)vm_read8(object + 0xAu) : 0u,
                    addr_readable(object + 0x124u)
                        ? vm_read32(object + 0x124u) : 0u,
                    addr_readable(object + 0x128u)
                        ? vm_read32(object + 0x128u) : 0u);
            fflush(stderr);
        }
    }
    /*
     * a010 model-submit frontier. func_00094DA0 finishes preparing each model
     * section through func_000A54A4 -> func_000DBC2C. The latter selects the
     * active renderer callback and reaches it at LR 0x000DBDC0 with r3 equal
     * to the prepared model-data object and r4 equal to the selected slot.
     * Capture the first few distinct targets/objects so an empty submission
     * is distinguishable from a later SPU command-builder failure.
     */
    {
        static int enabled = -1;
        if (enabled < 0)
            enabled = getenv("YZ_A010_MODELREAD") ? 1 : 0;
        if (enabled && g_yz_a010_root_active != 0 &&
            (uint32_t)ctx->lr == 0x000DBDC0u) {
            struct submit_call {
                uint32_t target;
                uint32_t object;
                unsigned hits;
            };
            static submit_call seen[128] = {};
            static unsigned seen_count;
            const uint32_t object = (uint32_t)ctx->gpr[3];
            unsigned slot = seen_count;
            for (unsigned i = 0; i < seen_count; i++) {
                if (seen[i].target == target && seen[i].object == object) {
                    slot = i;
                    break;
                }
            }
            if (slot == seen_count && seen_count < 128u)
                seen[seen_count++] = {target, object, 0u};
            if (slot < 128u) {
                submit_call& call = seen[slot];
                call.hits++;
                if (call.hits <= 4u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-model-submit] target=%08X object=%08X "
                            "slot=%08X hits=%u "
                            "f00=%08X f04=%08X f1C=%08X f20=%08X "
                            "f100=%08X f104=%08X\n",
                            target, object, (uint32_t)ctx->gpr[4], call.hits,
                            addr_readable(object + 0x00u)
                                ? vm_read32(object + 0x00u) : 0u,
                            addr_readable(object + 0x04u)
                                ? vm_read32(object + 0x04u) : 0u,
                            addr_readable(object + 0x1Cu)
                                ? vm_read32(object + 0x1Cu) : 0u,
                            addr_readable(object + 0x20u)
                                ? vm_read32(object + 0x20u) : 0u,
                            addr_readable(object + 0x100u)
                                ? vm_read32(object + 0x100u) : 0u,
                            addr_readable(object + 0x104u)
                                ? vm_read32(object + 0x104u) : 0u);
                    fflush(stderr);
                }
            }
        }
        const uint32_t submit_lr = (uint32_t)ctx->lr;
        const bool model_deep_site =
            submit_lr == 0x000D8800u ||
            submit_lr == 0x000D8954u ||
            submit_lr == 0x000D8E04u ||
            submit_lr == 0x000D8EB0u ||
            submit_lr == 0x000D8F70u ||
            submit_lr == 0x000D9064u;
        if (enabled && g_yz_a010_root_active != 0 && model_deep_site) {
            struct deep_submit_call {
                uint32_t lr;
                uint32_t target;
                unsigned hits;
            };
            static deep_submit_call seen[128] = {};
            static unsigned seen_count;
            unsigned slot = seen_count;
            for (unsigned i = 0; i < seen_count; i++) {
                if (seen[i].lr == submit_lr && seen[i].target == target) {
                    slot = i;
                    break;
                }
            }
            if (slot == seen_count && seen_count < 128u)
                seen[seen_count++] = {submit_lr, target, 0u};
            if (slot < 128u) {
                deep_submit_call& call = seen[slot];
                call.hits++;
                if (call.hits <= 8u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-model-deep] lr=%08X target=%08X hits=%u "
                            "r3=%08X r4=%08X r5=%08X r6=%08X "
                            "r3f00=%08X r3f04=%08X r3f08=%08X\n",
                            submit_lr, target, call.hits,
                            (uint32_t)ctx->gpr[3],
                            (uint32_t)ctx->gpr[4],
                            (uint32_t)ctx->gpr[5],
                            (uint32_t)ctx->gpr[6],
                            addr_readable((uint32_t)ctx->gpr[3])
                                ? vm_read32((uint32_t)ctx->gpr[3]) : 0u,
                            addr_readable((uint32_t)ctx->gpr[3] + 4u)
                                ? vm_read32((uint32_t)ctx->gpr[3] + 4u) : 0u,
                            addr_readable((uint32_t)ctx->gpr[3] + 8u)
                                ? vm_read32((uint32_t)ctx->gpr[3] + 8u) : 0u);
                    fflush(stderr);
                }
            }
        }
    }
    {
        static int enabled = -1;
        static unsigned hits;
        if (enabled < 0)
            enabled = getenv("YZ_A010_HANDOFF") ? 1 : 0;
        if (enabled && g_yz_a010_root_active != 0) {
            constexpr uint32_t manager_global = 0x014EC864u;
            const uint32_t manager = vm_read32(manager_global);
            const uint32_t auth = addr_readable(manager + 0x2B0u)
                                ? vm_read32(manager + 0x2B0u) : 0u;
            const uint32_t object = (uint32_t)ctx->gpr[3];
            if (object == auth || target == 0x00C942E8u ||
                target == 0x0134F4E8u) {
                hits++;
                if (hits <= 16u || (hits & (hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-root-call] hit=%u target=%08X lr=%08X "
                            "object=%08X auth=%08X state=%08X flags=%08X "
                            "tid=%u\n",
                            hits, target, (uint32_t)ctx->lr, object, auth,
                            addr_readable(auth + 0xFCu)
                                ? vm_read32(auth + 0xFCu) : ~0u,
                            addr_readable(auth + 0x280u)
                                ? vm_read32(auth + 0x280u) : 0u,
                            yz_thread_current_id());
                    fflush(stderr);
                }
            }
        }
    }
    /*
     * AUTH playback (state 16) enters func_00C970D4 from func_00C98028, then
     * func_00C970D4 performs the actual indirect track callbacks.  The old
     * filter started at 0x00C98028 and therefore measured only an armed banner:
     * every bctrl call site lives earlier, at 0x00C97188..0x00C97CA0 (plus
     * the small recursive list walker at 0x00C97FA8/0x00C97FE4).  Record each
     * distinct call-site/target/object tuple during a010 so a missing
     * renderer-facing track is distinguishable from a loaded track that
     * simply returns early.  Keep this diagnostic opt-in and bounded.
     */
    {
        static int enabled = -1;
        static int model_read_enabled = -1;
        static bool model_read_armed;
        if (model_read_enabled < 0)
            model_read_enabled = getenv("YZ_A010_MODELREAD") ? 1 : 0;
        if (model_read_enabled && !model_read_armed &&
            g_yz_a010_root_active != 0 &&
            target == 0x00015B9Cu &&
            (uint32_t)ctx->gpr[3] == 0x60992200u) {
            model_read_armed = true;
            yz_watch_arm_read(0x619C8290u);
        }
        struct auth_call {
            uint32_t scene;
            uint32_t lr;
            uint32_t target;
            uint32_t object;
            uint32_t hits;
        };
        static auth_call seen[1024] = {};
        static unsigned seen_count;
        static bool capture_started;
        if (enabled < 0) {
            enabled = getenv("YZ_A010_AUTHCB") ? 1 : 0;
            if (enabled) {
                fprintf(stderr,
                        "[a010-authcb] ARMED: indirect callbacks from "
                        "func_00C970D4/func_00C97F64; capture continues "
                        "into following AUTH scenes for comparison\n");
                fflush(stderr);
            }
        }
        if (enabled && g_yz_a010_root_active != 0)
            capture_started = true;
        const uint32_t lr = (uint32_t)ctx->lr;
        const bool auth_callback_site =
            (lr >= 0x00C97100u && lr < 0x00C97D00u) ||
            (lr >= 0x00C97F80u && lr < 0x00C98020u);
        if (enabled && capture_started && auth_callback_site) {
            const uint32_t object = (uint32_t)ctx->gpr[3];
            const uint32_t root = (uint32_t)ctx->gpr[31];
            const uint32_t scene =
                addr_readable(root + 0x100u) ? vm_read32(root + 0x100u) : 0u;
            unsigned slot = seen_count;
            for (unsigned i = 0; i < seen_count; i++) {
                if (seen[i].scene == scene && seen[i].lr == lr &&
                    seen[i].target == target &&
                    seen[i].object == object) {
                    slot = i;
                    break;
                }
            }
            if (slot == seen_count && seen_count < 1024u) {
                seen[seen_count++] = {scene, lr, target, object, 0u};
            }
            if (slot < 1024u) {
                auth_call& call = seen[slot];
                call.hits++;
                if (call.hits <= 4u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-authcb] scene=%08X root=%08X "
                            "lr=%08X target=%08X "
                            "object=%08X vtable=%08X hits=%u "
                            "r4=%08X r5=%08X r6=%08X time=%.3f\n",
                            scene, root, lr, target, object,
                            addr_readable(object) ? vm_read32(object) : 0u,
                            call.hits, (uint32_t)ctx->gpr[4],
                            (uint32_t)ctx->gpr[5],
                            (uint32_t)ctx->gpr[6],
                            (double)ctx->fpr[1]);
                    if (call.hits == 1u && addr_readable(object + 0xA9Cu)) {
                        fprintf(stderr,
                                "[a010-authcb-object] scene=%08X "
                                "target=%08X object=%08X "
                                "fields14=%08X 1C=%08X 20=%08X 24=%08X "
                                "28=%08X 2C=%08X 30=%08X 34=%08X 48=%08X "
                                "DC=%08X FC=%08X A78=%08X A80=%08X "
                                "A9C=%08X\n",
                                scene, target, object,
                                vm_read32(object + 0x14u),
                                vm_read32(object + 0x1Cu),
                                vm_read32(object + 0x20u),
                                vm_read32(object + 0x24u),
                                vm_read32(object + 0x28u),
                                vm_read32(object + 0x2Cu),
                                vm_read32(object + 0x30u),
                                vm_read32(object + 0x34u),
                                vm_read32(object + 0x48u),
                                vm_read32(object + 0xDCu),
                                vm_read32(object + 0xFCu),
                                vm_read32(object + 0xA78u),
                                vm_read32(object + 0xA80u),
                                vm_read32(object + 0xA9Cu));
                    }
                    if (target == 0x00011DF0u &&
                        addr_readable(object + 0x20u)) {
                        const uint32_t node = vm_read32(object + 0x20u);
                        const uint32_t track =
                            addr_readable(node) ? vm_read32(node) : 0u;
                        const uint32_t start_bits =
                            addr_readable(track + 0x1Cu)
                                ? vm_read32(track + 0x18u) : 0u;
                        const uint32_t end_bits =
                            addr_readable(track + 0x1Cu)
                                ? vm_read32(track + 0x1Cu) : 0u;
                        float start = 0.0f;
                        float end = 0.0f;
                        memcpy(&start, &start_bits, sizeof(start));
                        memcpy(&end, &end_bits, sizeof(end));
                        fprintf(stderr,
                                "[a010-camera-range] scene=%08X hit=%u "
                                "time=%.3f node=%08X track=%08X "
                                "range=%.3f..%.3f raw=%08X/%08X "
                                "next=%08X\n",
                                scene, call.hits, (double)ctx->fpr[1],
                                node, track, (double)start, (double)end,
                                start_bits, end_bits,
                                addr_readable(node + 0x4u)
                                    ? vm_read32(node + 0x4u) : 0u);
                        if (call.hits == 1u) {
                            const uint32_t heads[2] = {
                                addr_readable(object + 0x1Cu)
                                    ? vm_read32(object + 0x1Cu) : 0u,
                                node
                            };
                            const char* names[2] = {"inactive", "active"};
                            for (unsigned list_i = 0; list_i < 2u; list_i++) {
                                uint32_t camera_node = heads[list_i];
                                for (unsigned i = 0; i < 16u &&
                                     addr_readable(camera_node + 0xCu); i++) {
                                    const uint32_t camera_track =
                                        vm_read32(camera_node);
                                    uint32_t camera_start_bits = 0;
                                    uint32_t camera_end_bits = 0;
                                    float camera_start = 0.0f;
                                    float camera_end = 0.0f;
                                    if (addr_readable(camera_track + 0x1Cu)) {
                                        camera_start_bits =
                                            vm_read32(camera_track + 0x18u);
                                        camera_end_bits =
                                            vm_read32(camera_track + 0x1Cu);
                                        memcpy(&camera_start,
                                               &camera_start_bits,
                                               sizeof(camera_start));
                                        memcpy(&camera_end,
                                               &camera_end_bits,
                                               sizeof(camera_end));
                                    }
                                    fprintf(stderr,
                                            "[a010-camera-list] %s i=%u "
                                            "node=%08X track=%08X "
                                            "range=%.3f..%.3f "
                                            "raw=%08X/%08X next=%08X\n",
                                            names[list_i], i, camera_node,
                                            camera_track,
                                            (double)camera_start,
                                            (double)camera_end,
                                            camera_start_bits,
                                            camera_end_bits,
                                            vm_read32(camera_node +
                                                (list_i == 0u ? 0xCu : 0x4u)));
                                    camera_node =
                                        vm_read32(camera_node +
                                            (list_i == 0u ? 0xCu : 0x4u));
                                }
                            }
                        }
                    }
                    fflush(stderr);
                }
            }
        }

        /*
         * The AUTH callback above proves that a character track was scheduled,
         * but not how far func_00014848 progressed.  Record its indirect
         * model/controller calls as a compact path signature.  A missing
         * 0x14CB8 call means the character update returned before reaching its
         * bound model; later 0x14Dxx/0x150xx calls show which child/effect
         * submission branches were populated.
         */
        const bool character_deep_site =
            lr == 0x00014CB8u ||
            lr == 0x00014DFCu || lr == 0x00014E30u ||
             lr == 0x00014E84u || lr == 0x00014EB8u ||
             lr == 0x0001503Cu || lr == 0x00015084u ||
             lr == 0x000150B0u || lr == 0x000150DCu ||
             lr == 0x00015130u ||
             lr == 0x00015C84u || lr == 0x00015CB8u;
        if (enabled && capture_started && character_deep_site) {
            struct deep_call {
                uint32_t lr;
                uint32_t target;
                uint32_t hits;
            };
            static deep_call deep_seen[64] = {};
            static unsigned deep_seen_count;
            unsigned slot = deep_seen_count;
            for (unsigned i = 0; i < deep_seen_count; i++) {
                if (deep_seen[i].lr == lr &&
                    deep_seen[i].target == target) {
                    slot = i;
                    break;
                }
            }
            if (slot == deep_seen_count && deep_seen_count < 64u)
                deep_seen[deep_seen_count++] = {lr, target, 0u};
            if (slot < 64u) {
                deep_call& call = deep_seen[slot];
                call.hits++;
                if (call.hits <= 8u ||
                    (call.hits & (call.hits - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-char-deep] lr=%08X target=%08X hits=%u "
                            "r3=%08X r4=%08X r5=%08X r6=%08X\n",
                            lr, target, call.hits,
                            (uint32_t)ctx->gpr[3],
                            (uint32_t)ctx->gpr[4],
                            (uint32_t)ctx->gpr[5],
                            (uint32_t)ctx->gpr[6]);
                    fflush(stderr);
                }
            }
        }
    }
    if (YZ_DIAG_ENABLED("YZ_A010_TRACE") &&
        g_yz_a010_root_active != 0 &&
        (uint32_t)ctx->lr == 0x00F0025Cu) {
        const uint32_t request = (uint32_t)ctx->gpr[31];
        const uint32_t tracked =
            _InterlockedCompareExchange(
                (volatile long*)&g_yz_a010_open_request, 0, 0);
        static unsigned callback_n = 0;
        if (request == tracked || callback_n < 48u) {
            callback_n++;
            fprintf(stderr,
                    "[a010-load-callback] n=%u%s request=%08X tracked=%08X "
                    "target=%08X arg=%08X opd=%08X/%08X tid=%u\n",
                    callback_n, request == tracked ? " MATCH" : "",
                    request, tracked, target, (uint32_t)ctx->gpr[3],
                    addr_readable(target) ? vm_read32(target) : 0u,
                    addr_readable(target + 4u) ? vm_read32(target + 4u) : 0u,
                    yz_thread_current_id());
            fflush(stderr);
        }
    }
    if (yz_thread_current_id() == 1) {
        g_yz_t1_last_target = target;
        g_yz_t1_sample_seq++;
        /* s25 spin-witness feed (see g_yz_t1_last_tf in this file): guest
         * addr cast to the same slot the tramp path fills with host ptrs —
         * only ever compared for equality, and the two spaces can't collide. */
        extern volatile void* g_yz_t1_last_tf;
        g_yz_t1_last_tf = (void*)(uintptr_t)target;

        /* s24 (env YZ_UPDCB): the master per-object update handler
         * func_00D1E838 [0xD1E838,0xD1FC38) dispatches ~27 callbacks via bctrl
         * per main-loop iteration; the oracle completes every iteration in
         * ~40-65 ms while ours never returns from iteration #2 (chain-probe
         * census, scratch/s24cp1.err). Logging every bctrl issued FROM inside
         * it names the callback that never returns (= the last line printed).
         * Cap 400 lines (2-3 iterations' worth). */
        { static int ucb = -1; static int ucb_n = 0;
          if (ucb < 0) { ucb = getenv("YZ_UPDCB") ? 1 : 0;
              if (ucb) fprintf(stderr, "[updcb] ARMED: t1 bctrl-from-func_00D1E838 log\n"); }
          if (ucb && ucb_n < 400) {
              uint32_t l = (uint32_t)ctx->lr;
              if (l >= 0x00D1E838u && l < 0x00D1FC38u) {
                  ucb_n++;
                  fprintf(stderr, "[updcb] #%d target=0x%08X lr=0x%08X r3=0x%llX r4=0x%llX\n",
                          ucb_n, target, l,
                          (unsigned long long)ctx->gpr[3],
                          (unsigned long long)ctx->gpr[4]);
                  if (ucb_n == 400) fprintf(stderr, "[updcb] cap reached\n");
                  fflush(stderr);
              }
          }
        }
    }

    /* GENERIC indirect-call hook (env YZ_HOOK, 2026-07-02): comma-separated hex
     * guest code/OPD addresses (up to 8); logs args + lr on every bctrl to a
     * match. Replaces per-question recompiled probes like YZ_SIGCALL below.
     * LIMITATION: only sees INDIRECT calls (bctrl / function-table dispatch);
     * direct `bl` targets inside one lift never reach this dispatcher.
     * Example: YZ_HOOK=020125D8,020169D0  (names: scratch/libsre_lle_map.txt) */
    { static int hn = -1; static uint32_t hk[8];
      if (hn < 0) { hn = 0;
          const char* s = getenv("YZ_HOOK");
          while (s && *s && hn < 8) {
              char* end = NULL;
              unsigned long v = strtoul(s, &end, 16);
              if (end == s) break;
              hk[hn++] = (uint32_t)v;
              s = (*end == ',') ? end + 1 : end;
              if (*s == '\0') break;
          }
          /* s23: armed banner (a zero-hit boot without one is PLAUSIBLE, not
           * MEASURED -- this bit us on the 00DDDA6C hook). NB per the SIGCALL
           * comment below, a bctrl may carry the OPD address rather than the
           * code address: hook BOTH (find the OPD EA whose word0 = the code
           * addr) or expect false negatives. */
          if (hn) { fprintf(stderr, "[hook] armed:"); for (int i = 0; i < hn; i++)
                        fprintf(stderr, " 0x%08X", hk[i]);
                    fprintf(stderr, "\n"); fflush(stderr); } }
      for (int i = 0; i < hn; i++) {
          if (target == hk[i]) {
              static int n = 0; if (n < 60) { n++;
                  fprintf(stderr, "[hook] 0x%08X(r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX) lr=0x%llX\n",
                          target, (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                          (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6],
                          (unsigned long long)ctx->lr);
                  fflush(stderr); }
              break;
          }
      } }

    /* Observe callback transitions that pass through the indirect dispatcher. */
    if ((target == 0x00AF1F48u || target == 0x01347AB8u) &&
        !g_yz_cri_yield_phase) {
        g_yz_cri_yield_phase = 1;
        fprintf(stderr, "[cri-yield] phase gate enabled at title-to-menu callback\n");
        fflush(stderr);
    }
    if (target == 0x00AF1DD0u || target == 0x01347AB0u)
        yz_title_auto_start(ctx, 0x00AF1DD0u);
    if (target == 0x00AF24A4u || target == 0x01347AC8u)
        yz_auto_new_game_menu_input(ctx, target);
    switch (target) {
    case 0x00AF1FB0u: case 0x01347AC0u: yz_title_trace(ctx, 0x00AF1FB0u); break;
    case 0x00AF24A4u: case 0x01347AC8u: yz_title_trace(ctx, 0x00AF24A4u); break;
    case 0x00AF2F40u: case 0x01347AE8u: yz_title_trace(ctx, 0x00AF2F40u); break;
    case 0x00AF4CA0u: case 0x01347B60u: yz_title_trace(ctx, 0x00AF4CA0u); break;
    case 0x00AF53DCu: case 0x01347B78u: yz_title_trace(ctx, 0x00AF53DCu); break;
    case 0x00AF5614u: case 0x01347B80u: yz_title_trace(ctx, 0x00AF5614u); break;
    case 0x00AF56BCu: case 0x01347B90u: yz_title_trace(ctx, 0x00AF56BCu); break;
    case 0x00AF5860u:                       yz_title_trace(ctx, 0x00AF5860u); break;
    case 0x00AF0E10u: case 0x01347A58u: yz_title_trace(ctx, 0x00AF0E10u); break;
    case 0x00AF0FE4u: case 0x01347A60u: yz_title_trace(ctx, 0x00AF0FE4u); break;
    case 0x00AF11CCu: case 0x01347A68u: yz_title_trace(ctx, 0x00AF11CCu); break;
    case 0x00AF13E4u: case 0x01347A78u: yz_title_trace(ctx, 0x00AF13E4u); break;
    case 0x00AF1390u: case 0x01347A70u: yz_title_trace(ctx, 0x00AF1390u); break;
    case 0x00AF1DD0u: case 0x01347AB0u: yz_title_trace(ctx, 0x00AF1DD0u); break;
    case 0x00AF1F48u: case 0x01347AB8u: yz_title_trace(ctx, 0x00AF1F48u); break;
    default: break;
    }
    /* SPURS task-signal call watch (env YZ_SIGCALL, 2026-07-02, diag — REMOVE
     * when the voice frontier closes): all 6 SPU tasks park in WAIT_SIGNAL and
     * the tasksets' `signalled` bitmaps never set — does the PPU EVER call the
     * signal/queue-push family? Match both the LLE code addrs
     * (scratch/libsre_lle_map.txt) and their export OPD addrs (a bctrl may
     * carry either before the OPD fallback resolves). */
    { static int sw = -1; if (sw < 0) sw = getenv("YZ_SIGCALL") ? 1 : 0;
      if (sw) {
          const char* nm =
              (target == 0x020125D8u || target == 0x02031634u) ? "_cellSpursSendSignal" :
              (target == 0x020169D0u || target == 0x0203181Cu) ? "cellSpursQueuePushBody" :
              (target == 0x020171B8u)                          ? "_cellSpursLFQueuePushBody" :
              (target == 0x02016CFCu || target == 0x02031824u) ? "cellSpursQueuePopBody" : 0;
          if (nm) {
              static int n = 0; if (n < 40) { n++;
                  fprintf(stderr, "[sigcall] %s(r3=0x%llX r4=0x%llX r5=0x%llX) lr=0x%llX\n",
                          nm, (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                          (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->lr);
                  fflush(stderr); }
          }
      } }

    /* SPURS event-flag WAIT/SET watch (env YZ_EVFLAG_WATCH, 2026-07-04, diag —
     * REMOVE when the t1-wedge frontier closes). Measured t1 parked
     * forever in func_02015C2C (cellSpursEventFlagWait's poll body, entered via
     * func_02015F74) and the release test on object 0x63D61720 did NOT wake it
     * (scratch/adx_release_test2.err) -- so either the object, the waited
     * bitmask, or both are the wrong guess. This logs the REAL wait args t1
     * passes to func_02015F74 (object EA / mode / bitmask / the object's
     * current flag-bits at entry) and every cellSpursEventFlagSet call's
     * (object EA, set-bits, calling tid) so we can see whether a producer ever
     * targets t1's real object at all.
     *
     * ABI (read from the lifted bodies, recomp_prx/libsre_recomp_000.cpp):
     *   func_02015F74(eventFlag ea=r3, mode=r4, mask=r5) -- entry is a
     *     3-instruction trampoline (`gpr6=1; goto func_02015C2C`) that tail-
     *     hops into the real poll body func_02015C2C with the SAME r3/r4/r5
     *     (r3->r11/r9, r4->r31, r5 compared against 1 at loc_02015C88) and an
     *     implicit "wait" flag (r6=1) distinguishing it from the non-waiting
     *     entry func_02015F7C (r6=0, same body). The object's mode/type byte
     *     lives at [ea+0xE]; the CAS'd flag-bits FIELD is the 8-byte word at
     *     ea+0x0 itself (loc_02015CC4: gpr29 = (uint32_t)gpr9, and gpr9==gpr3
     *     ==the object ea unmodified -- the ldarx/stdcx pair at loc_02015D18/
     *     loc_02015D2C operates on *(u64*)ea, NOT ea+8). Log a 64-bit read at
     *     ea+0 as "current bits" (this also matches func_02016010's own CAS
     *     loop below, same ea+0 word).
     *   func_02016010(eventFlag ea=r3, bits=r4) -- cellSpursEventFlagSet; its
     *     ldarx/stdcx CAS loop (loc_020160F4/loc_020161E4) targets gpr27 =
     *     (uint32_t)gpr9 = (uint32_t)gpr3 = the SAME ea+0 word, confirming the
     *     two functions share one object layout. Confirmed against
     *     scratch/libsre_lle_map.txt (both names) and the ADX_SPURS_EVENTFLAG_*
     *     defines in libs/filesystem/cellFs.c (which independently arrived at
     *     the same two addresses). */
    if ((target == 0x02015F74u || target == 0x02016010u) &&
        yz_frontier_trace_is_armed()) {
        const uint32_t ea = (uint32_t)ctx->gpr[3];
        if (target == 0x02015F74u && yz_thread_current_id() == 1) {
            const uint32_t out = (uint32_t)ctx->gpr[4];
            const uint32_t mode = (uint32_t)ctx->gpr[5];
            const int ea_ok = ea >= 0x00010000u && ea < 0xE0000000u;
            const int out_ok = out >= 0x00010000u && out < 0xE0000000u;
            const uint64_t bits = ea_ok ? vm_read64(ea) : ~0ull;
            const uint32_t requested =
                out_ok ? (uint32_t)vm_read16(out) : 0xFFFFFFFFu;
            yz_frontier_trace_emit(
                YZ_FT_EVENT_WAIT, yz_thread_current_id(), target,
                ea, out, requested, mode,
                (uint32_t)(bits >> 32), (uint32_t)bits);
        } else if (target == 0x02016010u) {
            yz_frontier_trace_emit(
                YZ_FT_EVENT_SET, yz_thread_current_id(), target,
                ea, (uint32_t)ctx->gpr[4], (uint32_t)ctx->lr,
                0, 0, 0);
        }
    }

    { static int ew = -1; if (ew < 0) ew = getenv("YZ_EVFLAG_WATCH") ? 1 : 0;
      if (ew) {
          /* addr_readable() above only covers main-mem [0x10000,0x10000000) and
           * the stack region [0xD0000000,0xE0000000) -- it does NOT know about
           * the sys_memory-allocate heap (observed live at 0x40000000+, e.g.
           * "[sys_memory] allocate -> 0x40000000" early in every boot log),
           * which vm.h commits on demand (vm_commit, ppu/memory/vm.h ~157) and
           * is real, backed, readable guest memory. A local, WIDER guard here
           * (not touching the shared addr_readable, which other probes rely on
           * for its narrower main-mem/stack-only semantics) avoids a false
           * "EA UNREADABLE" on a perfectly valid SPURS-heap object. */
          auto evflag_ea_ok = [](uint32_t a) {
              return (a >= 0x00010000u && a < 0xE0000000u);
          };
          if (target == 0x02015F74u && yz_thread_current_id() == 1) {
              static long n = 0; long c = ++n;
              if (c <= 10 || (c % 97) == 0) {
                  uint32_t ea   = (uint32_t)ctx->gpr[3];
                  uint32_t mode = (uint32_t)ctx->gpr[4];
                  uint32_t mask = (uint32_t)ctx->gpr[5];
                  bool ok = evflag_ea_ok(ea);
                  uint64_t bits = ok ? vm_read64(ea) : ~0ull;
                  uint32_t type = ok ? vm_read8(ea + 0xEu) : 0xFFu;
                  fprintf(stderr, "[evflag-wait] #%ld tid=1 ea=0x%08X mode=0x%X mask=0x%08X "
                          "cur_bits=0x%016llX type@0xE=0x%02X %s lr=0x%08llX\n",
                          c, ea, mode, mask, (unsigned long long)bits, type,
                          ok ? "" : "(EA UNREADABLE)",
                          (unsigned long long)ctx->lr);
                  fflush(stderr);

                  /* YZ_EVFLAG_BT (2026-07-04, diag -- DIAGNOSIS TASK, remove with
                   * the t1-wedge frontier): the immediate ctx->lr here is 0 (this
                   * hook fires INSIDE the trampoline hop func_02015F74 -> the real
                   * poll body func_02015C2C, and the trampoline is reached via
                   * bctr, not bl -- so ctx->lr is whatever the trampoline itself
                   * had, not the GAME caller). Walk the guest PPC64 back-chain
                   * (r1 -> *(r1)=caller sp, saved LR at sp+0x10 per frame) to
                   * name the real game function(s) that called into this wait.
                   * Reuses the crash handler's yz_dump_guest_state walker
                   * (extern from main.cpp) -- only for ea==0x4019C680 (the
                   * measured wedge object) and only the first few hits, to keep
                   * output bounded (must not perturb the guest and must stay
                   * env-gated/default-off). */
                  { static int bt = -1; if (bt < 0) bt = getenv("YZ_EVFLAG_BT") ? 1 : 0;
                    static long btn = 0;
                    if (bt && ea == 0x4019C680u && btn < 5) {
                        btn++;
                        yz_dump_guest_state(ctx, "evflag-bt");
                    } }
              }
          } else if (target == 0x02016010u) {
              static long n = 0; long c = ++n;
              if (c <= 10 || (c % 97) == 0) {
                  uint32_t ea  = (uint32_t)ctx->gpr[3];
                  uint32_t set = (uint32_t)ctx->gpr[4];
                  fprintf(stderr, "[evflag-set] #%ld tid=%u ea=0x%08X set_bits=0x%08X lr=0x%08llX\n",
                          c, yz_thread_current_id(), ea, set, (unsigned long long)ctx->lr);
                  fflush(stderr);
              }
          }
      } }

    /* SPURS event-flag FORCE (env YZ_EVFLAG_FORCE, 2026-07-04, DIAGNOSTIC ONLY --
     * see docs/FLAGS.md; NOT a shipping fix, must stay env-gated and
     * default-off with a kill-switch). Purpose: t1 deadlocks
     * forever in cellSpursEventFlagWait on IWL object ea=0x4019C680, waiting for
     * mask 0x1; the owning SPU workload never signals it. This forces the wait
     * to succeed so we can observe what boot wall comes NEXT.
     *
     * Injection point: func_02015F74's entry (target==0x02015F74u), i.e. BEFORE
     * the trampoline hops into the real poll body func_02015C2C. func_02015C2C's
     * CAS loop (loc_02015D18/loc_02015D2C) operates on the FULL 64-bit word at
     * ea+0x0 (ldarx/stdcx, not a narrower access), and extracts the `events`
     * struct field (CellSpursEventFlag, RPCS3 cellSpurs.h:890, be_t<u16> at
     * ea+0x00) as the TOP 16 bits of that 64-bit big-endian word: loc_02015D48
     * does `ppc_rldicl(gpr9, 48, 48)` = rotate-left-48 (=rotate-right-16) then
     * keep bits[48:63], which un-rotates the original bits[0:15] into the low
     * 16 bits -- i.e. `(bits >> 48) & 0xFFFF` in a host uint64_t holding the
     * BE-loaded word. So writing the guest BE u16 at ea+0x0 (vm_write16, which
     * does the host->guest byteswap itself) is exactly the field the CAS
     * fast-path re-checks; no need to touch ea+0x2 (a different sub-field also
     * covered by the same 8-byte CAS but unrelated to the mask test at
     * loc_02015D48/loc_02015D7C).
     *
     * We fire this on EVERY entry (not gated to first-N like the watch above)
     * since t1 may re-wait after this one is satisfied and hit a sibling
     * event-flag -- each such wait must also be force-satisfied to keep
     * scoping how far the boot advances. Scoped to ea==0x4019C680 only (the
     * ONE measured wedge object; do not generalize to "always satisfy any
     * wait", which would erase the very information this experiment wants). */
    { static int ef = -1; if (ef < 0) ef = getenv("YZ_EVFLAG_FORCE") ? 1 : 0;
      if (ef && target == 0x02015F74u && yz_thread_current_id() == 1) {
          uint32_t ea = (uint32_t)ctx->gpr[3];
          if (ea == 0x4019C680u) {
              vm_write16(ea + 0x0u, 0x0001u);
              static long n = 0; long c = ++n;
              if (c <= 20 || (c % 97) == 0) {
                  fprintf(stderr, "[evflag-force] #%ld tid=1 ea=0x%08X wrote events=0x0001 "
                          "(pre-CAS force) lr=0x%08llX\n",
                          c, ea, (unsigned long long)ctx->lr);
                  fflush(stderr);
              }

              /* Fallback (env YZ_EVFLAG_FORCE, same flag): if the CAS write above
               * did NOT stop t1 from parking (i.e. the poll body already read the
               * old bits before this hook ran, or it fell through into the
               * blocking sys_event_queue_receive at syscall 0x82 -- see
               * func_02015C2C loc_02015E3C/loc_02015ECC), also push the event
               * queue directly so a receiver blocked in the syscall wakes up.
               * eventQueueId lives at ea+0x7C (RPCS3 cellSpurs.h:915,
               * be_t<u32>); read it guest-endian via vm_read32. source/data*
               * are dummy nonzero payload -- the CAS-based wait re-checks the
               * bits itself on wake, it doesn't trust the event payload. */
              uint32_t qid = vm_read32(ea + 0x7Cu);
              if (qid != 0) {
                  sys_event_queue_push_by_id(qid, 0x4556464Cu /*"EVFL"*/, 1, 0, 0);
              }
          }
      } }

    /* SPURS event-flag LIFECYCLE watch (env YZ_EVFLAG_LIFECYCLE, 2026-07-04, diag --
     * DIAGNOSIS TASK, remove with the t1-wedge frontier). Complements YZ_EVFLAG_WATCH
     * (which only sees Wait/Set). This hooks the CREATE/ATTACH side so we can name the
     * workload (wid) that OWNS a given event-flag object -- the producer-chain
     * question for the t1 wedge on ea=0x4019C680.
     *
     * ABI (read from the lifted bodies, recomp_prx/libsre_recomp_000.cpp):
     *   func_02015758 = _cellSpursEventFlagInitialize(eventFlag ea=r3, taskset/spurs=r4,
     *     direction=r5, clearMode=r6, ...=r7): gpr29=r3(ea), gpr30=r4(taskset/spurs ptr),
     *     gpr31=r5(direction: 0=clears via CAS same as Set, else compared -- this is
     *     CELL_SPURS_EVENT_FLAG_CLEAR_AUTO/MANUAL), gpr27=r6, gpr28=r7. It reads
     *     *(r4+0x74) at loc_02015830 -- taskset+0x74 = the owning wid field,
     *     so THIS is how we read off the owning wid when r4 is a taskset (not NULL/IWL).
     *     Writes mode/type bytes to ea+0xC/0xD/0xE/0xF (ea+0xE done at loc_0201586C:
     *     type = (r27<1) ? ... ; the direction-derived type constant).
     *   func_020158C4 = cellSpursEventFlagAttachLv2EventQueue(eventFlag ea=r3): gpr30=r3;
     *     reads ea+0xE (mode/type byte) and, on the isIwl path (type==3, loc_02015978+),
     *     reads ea+0x74 as an EA and calls through it (func_0200B244) -- so ea+0x74 on an
     *     ATTACHED flag is a live pointer/id, not a static wid; only USEFUL at INIT time
     *     (before attach rewrites it) do we get taskset+0x74=wid. Log both the INIT-time
     *     wid-read and the ATTACH-time ea+0xE type byte + ea+0x74 raw word so we can tell
     *     isIwl (type==3, taskset==NULL passed to Initialize) from taskset-owned (type!=3,
     *     wid = *(taskset+0x74)). */
    { static int el = -1; if (el < 0) el = getenv("YZ_EVFLAG_LIFECYCLE") ? 1 : 0;
      if (el) {
          auto ok = [](uint32_t a) { return (a >= 0x00010000u && a < 0xE0000000u); };
          if (target == 0x02015758u) {
              static long n = 0; long c = ++n;
              uint32_t ea      = (uint32_t)ctx->gpr[3];
              uint32_t taskset = (uint32_t)ctx->gpr[4];
              uint32_t dir     = (uint32_t)ctx->gpr[5];
              bool tsOk = ok(taskset);
              uint32_t wid = (tsOk && taskset != 0) ? vm_read32(taskset + 0x74u) : 0xFFFFFFFFu;
              fprintf(stderr, "[evflag-init] #%ld tid=%u ea=0x%08X taskset=0x%08X dir=0x%X "
                      "%s wid(taskset+0x74)=%s0x%08X lr=0x%08llX\n",
                      c, yz_thread_current_id(), ea, taskset, dir,
                      (taskset == 0) ? "(NULL taskset => IWL)" : "",
                      (taskset == 0) ? "N/A=" : "",
                      wid, (unsigned long long)ctx->lr);
              fflush(stderr);
          } else if (target == 0x020158C4u) {
              static long n = 0; long c = ++n;
              uint32_t ea = (uint32_t)ctx->gpr[3];
              bool eaOk = ok(ea);
              uint32_t type = eaOk ? vm_read8(ea + 0xEu) : 0xFFu;
              uint32_t f74  = eaOk ? vm_read32(ea + 0x74u) : 0xFFFFFFFFu;
              fprintf(stderr, "[evflag-attach] #%ld tid=%u ea=0x%08X type@0xE=0x%02X "
                      "raw@0x74=0x%08X lr=0x%08llX\n",
                      c, yz_thread_current_id(), ea, type, f74, (unsigned long long)ctx->lr);
              fflush(stderr);
          } else if (target == 0x02015AA4u) {
              static long n = 0; long c = ++n;
              uint32_t ea = (uint32_t)ctx->gpr[3];
              fprintf(stderr, "[evflag-detach] #%ld tid=%u ea=0x%08X lr=0x%08llX\n",
                      c, yz_thread_current_id(), ea, (unsigned long long)ctx->lr);
              fflush(stderr);
          }
      } }

    /* LAYER-1 RECURSION PROBE (env YZ_RECPROBE, pt48b): the t7 _gcm_intr_thread
     * host-stack-overflows through the gcm ring; func_00EDC6B0 is bctrl'd on the
     * path. Watch the GUEST r1 trend across successive bctrl hits to this target:
     * a MONOTONIC decrease == genuine recursion (each level nests a host frame),
     * an oscillation == flat loop (overflow is elsewhere). After N hits, dump the
     * guest back-chain (r1 -> *(r1)=caller SP, saved LR at SP+0x10) = the exact
     * recursive cycle, captured BEFORE the host stack dies. One-shot. */
    if (target == 0x00F83AECu && YZ_DIAG_ENABLED("YZ_RECPROBE")) {
        static __declspec(thread) uint32_t prev_r1 = 0;
        static __declspec(thread) uint32_t hits = 0, dec_run = 0;
        static __declspec(thread) int dumped = 0;
        uint32_t r1 = (uint32_t)ctx->gpr[1];
        uint32_t obj = (uint32_t)ctx->gpr[3];
        hits++;
        if (prev_r1 && r1 < prev_r1) dec_run++; else dec_run = 0;
        prev_r1 = r1;
        if ((hits & 31u) == 1u)
            fprintf(stderr, "[recprobe] tid=%u hit#%u r1=0x%08X dec_run=%u obj(r3)=0x%08X\n",
                    yz_thread_current_id(), hits, r1, dec_run, obj);
        if (!dumped && dec_run >= 80u) {
            dumped = 1;
            fprintf(stderr, "[recprobe] *** RUNAWAY recursion on tid=%u: r1 fell %u steps to 0x%08X. "
                    "obj(r3)=0x%08X\n", yz_thread_current_id(), dec_run, r1, obj);
            if (obj >= 0x10000u && obj < 0xE0000000u) {
                fprintf(stderr, "[recprobe] obj dump:");
                for (uint32_t o = 0; o <= 0x60u; o += 4u) fprintf(stderr, " +%02X=%08X", o, vm_read32(obj + o));
                fprintf(stderr, "\n");
                uint32_t l = vm_read32(obj + 0x5Cu);    /* func_00F83A10 reads obj[0x5C] */
                fprintf(stderr, "[recprobe] obj[0x5C]=0x%08X", l);
                if (l >= 0x10000u && l < 0xE0000000u)
                    for (uint32_t o = 0; o <= 0x18u; o += 4u) fprintf(stderr, " *(+%02X)=%08X", o, vm_read32(l + o));
                fprintf(stderr, "\n");
            }
            uint32_t sp = r1;
            for (int f = 0; f < 24 && sp >= 0x10000u && sp < 0xE0000000u; f++) {
                uint32_t nxt = vm_read32(sp);
                uint32_t lr  = (nxt >= 0x10000u && nxt < 0xE0000000u) ? vm_read32(nxt + 0x10u) : 0;
                fprintf(stderr, "[recprobe]   frame %2d sp=0x%08X savedLR=0x%08X\n", f, sp, lr);
                if (nxt <= sp) break;
                sp = nxt;
            }
            fflush(stderr);
        }
    }

    /* DIAG (2026-06-16, drain trigger, env YZ_CB_TRACE): does libgcm fire the game's
     * gcm CONTEXT CALLBACK (gcm_ctx+0xC = OPD 0x01355208 -- the drain lives inside it)
     * on our per-segment buffer-fills, and does the op-list drain across it? RPCS3 (same
     * LLE code) drains continuously here; if ours never fires, that's the fixable gap. */
    { static int en = -1; if (en < 0) en = getenv("YZ_CB_TRACE") ? 1 : 0;
      if (en) {
        static uint32_t cb_code = 0; static int cbi = 0;
        if (!cbi) { cbi = 1; cb_code = vm_read32(0x01355208u); }
        if (cb_code && target == cb_code) {
            uint32_t depth = 0;
            if (g_yz_game_toc) { uint32_t S = vm_read32(g_yz_game_toc - 0x7410u);
                if (S >= 0x10000u && S < 0xE0000000u) {
                    uint32_t base = vm_read32(S+8u), head = vm_read32(S+0u);
                    if (head >= base && head-base <= 0x40000u) depth = (head-base)/0x20u; } }
            static long n = 0;
            fprintf(stderr, "[cb] gcm context-callback fired #%ld tid=%u oplist=%u\n",
                    ++n, yz_thread_current_id(), depth);
        }
      } }

    /* DIAG (env YZ_TASK_TRACE, 2026-06-16b): identify the game's render SPU task.
     * cellSpursCreateTask2WithBinInfo (libsre OPD 0x0203161C) args: r3=taskset2,
     * r4=taskId(out), r5=binInfo, r6=arg, r7=attr. binInfo->eaElf (u64 @+0) is the
     * task's SPU ELF ea -> the image to LIFT for 1f (SPU task dispatch). Log it +
     * the eaElf words so we know which of the EBOOT's 10 SPU images it is. */
    { static int en = -1; if (en < 0) en = getenv("YZ_TASK_TRACE") ? 1 : 0;
      if (en) {
        static uint32_t t2_code = 0; static int t2i = 0;
        if (!t2i) { t2i = 1; t2_code = vm_read32(0x0203161Cu); }
        if (target == 0x0203161Cu || (t2_code && target == t2_code)) {
            g_yz_spurs_taskset = (uint32_t)ctx->gpr[3];   /* for the SPURS-state dump */
            uint32_t bi = (uint32_t)ctx->gpr[5];
            uint64_t eaElf = bi ? ((uint64_t)vm_read32(bi) << 32 | vm_read32(bi + 4)) : 0;
            uint32_t elf = (uint32_t)eaElf;
            fprintf(stderr, "[task] CreateTask2WithBinInfo taskset=0x%llX taskId=0x%llX "
                    "binInfo=0x%X arg=0x%llX -> eaElf=0x%08X (w0=0x%08X w1=0x%08X)\n",
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4], bi,
                    (unsigned long long)ctx->gpr[6], elf,
                    elf ? vm_read32(elf) : 0, elf ? vm_read32(elf + 4) : 0);
            fflush(stderr);
        }
      } }

    /* DIAG (env YZ_TASK_TRACE, pt31): broaden beyond CreateTask2WithBinInfo -- catch
     * ALL SPURS task/taskset creation so we can see if the cri_audio decode task
     * (ELF 0x012B4980) is ever created in ANY taskset (e.g. the late wid-3 taskset).
     * Logs the ELF EA whether it's r5 directly (CreateTask) or inside a struct. */
    { static int en2 = -1; if (en2 < 0) en2 = getenv("YZ_TASK_TRACE") ? 1 : 0;
      if (en2) {
        static const struct { uint32_t opd; const char* nm; } creates[] = {
            {0x0203162Cu, "CreateTask"}, {0x02031624u, "CreateTaskWithAttr"},
            {0x02031764u, "CreateTaskset"}, {0x02031784u, "CreateTaskset2"},
            {0x0203175Cu, "CreateTasksetWithAttr"},
        };
        for (unsigned i = 0; i < 5; i++) {
            uint32_t code = vm_read32(creates[i].opd);
            if (target == creates[i].opd || (code && target == code)) {
                uint32_t r5 = (uint32_t)ctx->gpr[5];
                fprintf(stderr, "[task+] %s r3=0x%llX r4=0x%llX r5=0x%08X r6=0x%llX",
                        creates[i].nm, (unsigned long long)ctx->gpr[3],
                        (unsigned long long)ctx->gpr[4], r5, (unsigned long long)ctx->gpr[6]);
                uint32_t elf_in_attr = 0;
                if (r5 >= 0x10000u && r5 < 0xE0000000u) {
                    fprintf(stderr, " attr[");
                    for (int w = 0; w < 8; w++) { uint32_t v = vm_read32(r5 + (uint32_t)w*4u);
                        fprintf(stderr, "%s0x%08X", w?" ":"", v);
                        if (v == 0x012B4980u) elf_in_attr = 1; }
                    fprintf(stderr, "]");
                }
                if (r5 == 0x012B4980u || elf_in_attr) {
                    fprintf(stderr, "  <== cri_audio CODEC TASK");
                    g_yz_codec_taskset = (uint32_t)ctx->gpr[3];  /* CreateTaskWithAttr r3 = taskset */
                    { static int once = 0; if (!once) { once = 1;
                        fprintf(stderr, "\n[create-fn] CreateTaskWithAttr code=0x%08X CreateTask code=0x%08X CreateTask2 code=0x%08X\n",
                                vm_read32(0x02031624u), vm_read32(0x0203162Cu), vm_read32(0x0203161Cu)); fflush(stderr); } }
                    /* pt35: is a valid SPU ELF actually present at the codec eaElf?
                     * spursTasksetLoadElf DMAs+parses it; if it's missing/garbage,
                     * LoadElf fails -> spursHalt -> dispatch dies after SELECT (the
                     * stuck-running symptom). Expect magic 0x7F454C46 ('\x7fELF'). */
                    uint32_t eaElf = 0x012B4980u;
                    fprintf(stderr, " | eaElf=0x%08X bytes: %08X %08X %08X (magic %s)",
                            eaElf, vm_read32(eaElf), vm_read32(eaElf+4u), vm_read32(eaElf+8u),
                            vm_read32(eaElf) == 0x7F454C46u ? "OK" : "MISSING/BAD");
                }
                /* pt35: dump wid3's scheduling fields AT creation. SPURS @ 0x40197C80:
                 * wklReadyCount1@+0x00, wklCurrentContention@+0x20, wklMaxContention@+0x50
                 * (each u8[16], BE -> word holds wid0..3). If curCont byte for wid3 is
                 * already nonzero here, it's a bad INIT; if 0, it goes 1 later (stuck). */
                { uint32_t sp = 0x40197C80u;
                  fprintf(stderr, " | SPURS curCont@20=0x%08X rc@00=0x%08X maxC@50=0x%08X",
                          vm_read32(sp+0x20u), vm_read32(sp+0x00u), vm_read32(sp+0x50u)); }
                fprintf(stderr, "\n"); fflush(stderr);
                break;
            }
        }
      } }

    /* DIAG (env YZ_TASK_RET, 2026-06-20 pt27): the CRI codec SPURS task is created
     * (CreateTask2WithBinInfo) but never dispatches -- the taskset shows enabled=0,
     * so either the create RETURNS AN ERROR (0x80410902 INVAL / 0x80410911 NULL_PTR)
     * before allocating, or it succeeds but the enable doesn't land. Capture the
     * decisive datum: run the real create here (direct-call idiom, like
     * yz_call_guest_opd) and log its return value + the taskset bitsets right after,
     * one-shot. Then suppress the normal dispatch (we already executed it). */
    { static int rc = -1; static uint32_t rc_ts = 0;
      if (rc < 0) { const char* e = getenv("YZ_TASK_RET"); rc = e ? 1 : 0;
          /* 2026-07-03 s7: optional hex TASKSET filter (YZ_TASK_RET=40199D00)
           * so the one-shot fires on the FAILING create (the pxd IO-service
           * retry loop), not the first successful shader-phase one. "1" or
           * any non-hex value = fire on the first call as before. */
          if (e) { char* end = NULL; unsigned long v = strtoul(e, &end, 16);
                   if (end != e && v > 0xFFFFu) rc_ts = (uint32_t)v; } }
      if (rc) {
        static uint32_t t2c = 0; static int t2ci = 0;
        if (!t2ci) { t2ci = 1; t2c = vm_read32(0x0203161Cu); }
        /* Filtered mode (YZ_TASK_RET=<taskset hex>): NON-INVASIVE per-call
         * snapshot of the sendWorkloadSignal guard inputs at the create's
         * instant (2026-07-03: the signal's rc is DISCARDED by Sony's helper
         * 0x2010C6C -- a guard bail is invisible in the create's rc, and the
         * one-shot inline capture only ever saw the FIRST, healthy create).
         * Guards per libsre 0x200A750: wklEnabled bit (spurs+0xB0),
         * wklState1[wid]==2 (spurs+0x80+wid), spurs+0xD6C==0, wid<max. */
        if (t2c && (target == 0x0203161Cu || target == t2c)
                && rc_ts && (uint32_t)ctx->gpr[3] == rc_ts) {
            static int n = 0;
            if (n < 40) { n++;
                uint32_t ts    = rc_ts;
                uint32_t spurs = vm_read32(ts + 0x64u);
                uint32_t wid   = vm_read32(ts + 0x74u);
                uint32_t st    = (wid < 16u) ? vm_read8(spurs + 0x80u + wid) : 0xFFu;
                fprintf(stderr, "[task-crt] #%d ts=%08X spurs=%08X wid=%u state=%02X "
                        "enabled=%08X sig=%08X d6c=%08X | pend=%08X enb=%08X run=%08X\n",
                        n, ts, spurs, wid, st,
                        vm_read32(spurs + 0xB0u), vm_read32(spurs + 0x70u),
                        vm_read32(spurs + 0xD6Cu),
                        vm_read32(ts + 0x20u), vm_read32(ts + 0x30u), vm_read32(ts + 0x00u));
                fflush(stderr);
            }
            /* fall through: DO NOT run the create inline -- normal dispatch */
        }
        else if (t2c && (target == 0x0203161Cu || target == t2c) && !rc_ts) {
            static int once = 0;
            if (!once) { once = 1;
                uint32_t ts   = (uint32_t)ctx->gpr[3];   /* taskset2 */
                uint32_t tidp = (uint32_t)ctx->gpr[4];   /* taskId out ptr */
                uint32_t enb  = vm_read32(ts + 0x30u);
                yz_ppu_fn cf = yz_lookup_func(t2c);
                if (cf) {
                    ctx->gpr[2] = vm_read32(0x0203161Cu + 4u);   /* libsre TOC */
                    cf(ctx);
                    yz_drain_trampolines(ctx);
                    fprintf(stderr, "[task-ret] CreateTask2 ret=0x%08X taskId=0x%X | "
                            "enabled %08X->%08X pending_ready=%08X ready=%08X signalled=%08X wid=%u\n",
                            (uint32_t)ctx->gpr[3], tidp ? vm_read32(tidp) : 0xFFFFFFFFu, enb,
                            vm_read32(ts + 0x30u), vm_read32(ts + 0x20u),
                            vm_read32(ts + 0x10u), vm_read32(ts + 0x40u), vm_read32(ts + 0x74u));
                    fflush(stderr);
                    g_trampoline_fn = nullptr;
                    return;
                }
            }
        }
      } }

    /* DIAG (vblank-dispatch hunt): the gcm interrupt thread (t7) runs inside
     * libgcm (0x021xxxxx); if it makes an indirect CALL into GAME range it is
     * dispatching the game's registered vblank/flip handler. Catch it -- this
     * proves whether the handler actually fires per interrupt. */
    if (yz_thread_current_id() == 7 && target >= 0x10000u && target < 0x02000000u) {
        static int n = 0;
        if (n < 60) { n++;
            fprintf(stderr, "[diag t7] intr -> GAME handler dispatch target=0x%08X "
                    "(r3=0x%llX r4=0x%llX)\n", target,
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4]);
            fflush(stderr);
        }
    }
    /* DIAG (flip-wait hunt): capture WHERE the render thread (t1) calls the
     * vblank-pump 0x00E7ED8C from -- ctx->lr is the return addr = t1's poll
     * loop. That loop is the busy-poll we need to compare to RPCS3's semaphore
     * wait. Also log t1's other indirect targets to map the loop body. */
    if (yz_thread_current_id() == 1 && target == 0x00E7ED8Cu) {
        static int n = 0;
        if (n < 20) { n++;
            fprintf(stderr, "[diag t1] pump 0xE7ED8C from lr=0x%08llX r3=0x%llX\n",
                    (unsigned long long)ctx->lr, (unsigned long long)ctx->gpr[3]);
            fflush(stderr);
        }
    }


    /* MEASUREMENT (env YZ_PHASE): GET/PUT at each libgcm segment-recycle reserve
     * entry (func_02103AAC). Decisive test of whether the producer laps the
     * consumer: GET should track PUT (gap small / within one segment). If GET
     * falls a full segment behind over successive reserves, the producer is still
     * lapping. */
    if (target == 0x02103AACu) {
        static int phase = -1;
        if (phase < 0) phase = getenv("YZ_PHASE") ? 1 : 0;
        if (phase) {
            uint32_t get = vm_read32(0x10000044u) & ~3u;
            uint32_t put = vm_read32(0x10000040u) & ~3u;
            static int n = 0;
            if (n < 240) { n++;
                fprintf(stderr, "[reserve #%d] GET=0x%06X PUT=0x%06X\n", n, get, put);
                fflush(stderr); }
        }
    }

    /* Bridges complete entirely on the host, so call directly. */
    if (yz_ppu_fn bridge = import_bridge_for(target)) {
        /* TEMP DEBUG (SPURS bring-up): trace every import call made by the
         * LLE firmware module (indices >= g_yz_lle_import_first) with args
         * and result. Strip once SPURS init survives. */
        unsigned idx = (target - YZ_IMPORT_FAKE_BASE) / 4;
        /* s40b: the "TEMP DEBUG (SPURS bring-up)" trace above was never stripped
         * and is UNGATED — measured firing thousands of times/min on t10's
         * memcpy loop in the current boot phase, two stderr writes per import
         * call on the hot dispatch path (LESSONS #6c: the logging floor is an
         * instrument). Now env-gated OFF; YZ_LLECALL_TRACE=1 restores it. */
        static int lle_trace_on = -1;
        if (lle_trace_on < 0) lle_trace_on = getenv("YZ_LLECALL_TRACE") ? 1 : 0;
        int trace = lle_trace_on && target != YZ_GCM_CB_FAKE_KEY &&
                    idx >= g_yz_lle_import_first && idx < g_yz_import_count;
        if (target != YZ_GCM_CB_FAKE_KEY &&
            idx < g_yz_import_count &&
            strcmp(g_yz_import_names[idx], "sys_fs::cellFsOpen") == 0) {
            const uint32_t path_ea = (uint32_t)ctx->gpr[3];
            const char* path = addr_readable(path_ea)
                             ? (const char*)(vm_base + path_ea) : nullptr;
            if (YZ_DIAG_ENABLED("YZ_A010_TRACE") && path &&
                strstr(path, "/auth/a010/")) {
                const uint32_t request = (uint32_t)ctx->gpr[5] - 8u;
                _InterlockedExchange(
                    (volatile long*)&g_yz_a010_open_request, request);
                fprintf(stderr,
                        "[a010-open-call] tid=%u lr=%08X path_ea=%08X "
                        "r4=%08X r5=%08X r6=%08X request=%08X path='%s'\n",
                        yz_thread_current_id(), (uint32_t)ctx->lr, path_ea,
                        (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                        (uint32_t)ctx->gpr[6], request, path);
                if (addr_readable(request + 0x5Cu)) {
                    fprintf(stderr, "[a010-open-request] %08X:", request);
                    for (uint32_t off = 0; off <= 0x5Cu; off += 4u) {
                        if ((off & 0x1Fu) == 0u)
                            fprintf(stderr, "\n[a010-open-request]   +%02X:",
                                    off);
                        fprintf(stderr, " %08X", vm_read32(request + off));
                    }
                    fprintf(stderr, "\n");
                }
                yz_dump_guest_state(ctx, "a010-open-call");
                fflush(stderr);
            }
        }
        if (trace)
            fprintf(stderr, "[lle-call t%u] %s(r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX)\n",
                    yz_thread_current_id(), g_yz_import_names[idx],
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
                    (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6]);
        bridge(ctx);
        yz_stage_gmd_allocator_return(stage_gmd_allocator, ctx);
        if (trace) {
            fprintf(stderr, "[lle-call t%u]   -> 0x%llX\n",
                    yz_thread_current_id(), (unsigned long long)ctx->gpr[3]);
            fflush(stderr);
        }
        return;
    }

    uint32_t resolved_code = target;
    yz_ppu_fn fn = yz_lookup_func(target);
    /* TOC repair: a gcm callback invoked by its bare code address (target is a
     * game function found directly, no OPD) keeps the CALLER's TOC (libgcm's, or
     * 0). The game module has exactly one TOC, so force it for any game target. */
    if (fn && target < 0x02000000u && g_yz_game_toc && ctx->gpr[2] != g_yz_game_toc)
        ctx->gpr[2] = g_yz_game_toc;
    if (!fn && addr_readable(target)) {
        /* OPD fallback: ctr may hold a descriptor address rather than code.
         * The descriptor's code word may itself be an import-bridge key
         * (our synthetic OPDs, or game-held pointers to import slots). */
        uint32_t code = vm_read32(target);
        uint32_t toc  = vm_read32(target + 4);
        resolved_code = code;
        if (yz_ppu_fn bridge = import_bridge_for(code)) {
            bridge(ctx);
            yz_stage_gmd_allocator_return(stage_gmd_allocator, ctx);
            return;
        }
        fn = yz_lookup_func(code);
        if (fn) {
            ctx->gpr[2] = toc;
            /* Same TOC repair as the direct path: a game-range fn reached via an OPD
             * whose TOC word is 0 would run with r2=0 and fault on its first TOC load. */
            if (code < 0x02000000u && g_yz_game_toc && ctx->gpr[2] != g_yz_game_toc)
                ctx->gpr[2] = g_yz_game_toc;
        }
    }

    if (!fn) {
        fprintf(stderr,
                "[dispatch] no host function for guest target 0x%08X "
                "(lr=0x%08llX r2=0x%08llX r3=0x%08llX)\n",
                target,
                (unsigned long long)ctx->lr,
                (unsigned long long)ctx->gpr[2],
                (unsigned long long)ctx->gpr[3]);
        exit(2);
    }

    if (resolved_code == 0x00E7DB10u) {
        /*
         * The callback signals the wid4 SPU before it finishes staging that
         * signal's work record.  Execute this one callback to return here so
         * the DMA side can wait for a real publication acknowledgement rather
         * than an arbitrary spin count.
        */
        const long cause = (long)(uint32_t)ctx->gpr[3];
        const long epoch = yz_ucmd_handler_begin(cause);
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        yz_ucmd_handler_end(cause, epoch);
        g_trampoline_fn = saved_trampoline;
        return;
    }

    yz_mwply_lifecycle_boundary((void*)fn, ctx);

    /* DYNAMIC PROBE (env YZ_MWPLY_PROBE, 2026-07-04): live-ABI instrumentation
     * for the CRI Sofdec mwPly player (see scratch/MWPLY_RESOLVE.md). Covers
     * the genuinely-indirect (bctr through the function table) call path with
     * FULL entry+exit+out-param logging. NOTE: at least one of the three
     * (func_00F48E48) is ALSO reached via a same-chunk direct tail-branch
     * (`g_trampoline_fn = func_00F48E48; return;` inside another lifted
     * function) which never reaches ps3_indirect_call at all -- that path is
     * covered separately (entry-args-only) in yz_tramp_guard below, since the
     * DRAIN_TRAMPOLINE macro captures the callee into a local before any hook
     * runs and calls it unconditionally, so a post-call wrapper can't be
     * spliced in there without editing generated code. */
    if (yz_mwply_probe_target(target)) {
        yz_mwply_probe_dispatch(target, fn, ctx);
        return;
    }

    /*
     * Compact a010 character-update transition trace.
     *
     * The model palette at +0x218 is a transient render allocation and is
     * expected to move between frames.  The durable animation source is the
     * matrix array at model+0x214.  Run the ordinary virtual update
     * synchronously only while this opt-in probe is enabled, then report the
     * first few calls and the first transition from the Y=10000 parking pose
     * to a finite authored pose.  This preserves the normal guest routine and
     * avoids another enormous per-call boot log.
     */
    if (YZ_DIAG_ENABLED("YZ_A010_TRANSITION_TRACE") &&
        target == 0x00014848u &&
        active_a010_root != 0u &&
        addr_readable(dispatch_object + 0x2Cu) &&
        vm_read32(dispatch_object) == 0x011D3F78u) {
        struct yz_a010_update_transition {
            uint32_t object;
            uint32_t last_matrix_y;
            uint32_t last_model_flags;
            unsigned calls;
            bool authored_seen;
        };
        static yz_a010_update_transition records[16] = {};
        static unsigned record_count = 0;
        unsigned slot = record_count;
        for (unsigned i = 0; i < record_count; ++i) {
            if (records[i].object == dispatch_object) {
                slot = i;
                break;
            }
        }
        if (slot == record_count && record_count < 16u)
            records[record_count++] =
                {dispatch_object, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, false};

        yz_a010_update_transition* record =
            slot < 16u ? &records[slot] : nullptr;
        const uint32_t model = vm_read32(dispatch_object + 0x2Cu);
        const uint32_t matrices =
            addr_readable(model + 0x214u)
                ? vm_read32(model + 0x214u) : 0u;
        const uint32_t palette =
            addr_readable(model + 0x218u)
                ? vm_read32(model + 0x218u) : 0u;
        const uint32_t before_matrix_y =
            addr_readable(matrices + 0x34u)
                ? vm_read32(matrices + 0x34u) : 0xFFFFFFFFu;
        const uint32_t before_palette_y =
            addr_readable(palette + 0x1Cu)
                ? vm_read32(palette + 0x1Cu) : 0xFFFFFFFFu;
        const uint32_t before_model_flags =
            addr_readable(model + 0x10u)
                ? vm_read32(model + 0x10u) : 0xFFFFFFFFu;
        const float timeline =
            yz_read_float(active_a010_root + 0x210u);

        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        g_trampoline_fn = saved_trampoline;

        const uint32_t after_matrices =
            addr_readable(model + 0x214u)
                ? vm_read32(model + 0x214u) : 0u;
        const uint32_t after_palette =
            addr_readable(model + 0x218u)
                ? vm_read32(model + 0x218u) : 0u;
        const uint32_t after_matrix_y =
            addr_readable(after_matrices + 0x34u)
                ? vm_read32(after_matrices + 0x34u) : 0xFFFFFFFFu;
        const uint32_t after_palette_y =
            addr_readable(after_palette + 0x1Cu)
                ? vm_read32(after_palette + 0x1Cu) : 0xFFFFFFFFu;
        const uint32_t after_model_flags =
            addr_readable(model + 0x10u)
                ? vm_read32(model + 0x10u) : 0xFFFFFFFFu;
        float matrix_y = 0.0f;
        float palette_y = 0.0f;
        memcpy(&matrix_y, &after_matrix_y, sizeof(matrix_y));
        memcpy(&palette_y, &after_palette_y, sizeof(palette_y));
        const bool authored =
            std::isfinite(matrix_y) && std::fabs(matrix_y) < 1000.0f;

        bool report = false;
        if (record) {
            ++record->calls;
            report =
                record->calls <= 4u ||
                (authored && !record->authored_seen) ||
                after_model_flags != record->last_model_flags;
            record->last_matrix_y = after_matrix_y;
            record->last_model_flags = after_model_flags;
            record->authored_seen = record->authored_seen || authored;
        }
        if (report) {
            FILE* trace =
                fopen("scratch\\a010_character_transitions.log", "a");
            if (trace) {
                fprintf(trace,
                        "update=%u object=%08X time=%.3f result=%08X "
                        "model=%08X matrices=%08X->%08X "
                        "palette=%08X->%08X "
                        "matrixY=%08X->%08X(%.6f) "
                        "paletteY=%08X->%08X(%.6f) "
                        "modelFlags=%08X->%08X "
                        "visible=%08X objectFlags=%08X "
                        "head=%08X direct20=%08X authored=%u\n",
                        record ? record->calls : 0u,
                        dispatch_object, (double)timeline,
                        (uint32_t)ctx->gpr[3], model,
                        matrices, after_matrices,
                        palette, after_palette,
                        before_matrix_y, after_matrix_y, (double)matrix_y,
                        before_palette_y, after_palette_y, (double)palette_y,
                        before_model_flags, after_model_flags,
                        vm_read32(dispatch_object + 0x28u),
                        vm_read32(dispatch_object + 0x48u),
                        vm_read32(dispatch_object + 0x1Cu),
                        vm_read32(dispatch_object + 0x20u),
                        authored ? 1u : 0u);
                fclose(trace);
            }
        }
        return;
    }

    /*
     * func_00DA45E8 indexes a scene-object array and synchronously runs the
     * common visibility/classification routine (func_000E0BC8).  Static-stage
     * render methods later gate on B0 bits 0x0008, 0x0100, and 0x0400.  Keep
     * the repair at this game-owned boundary so it neither edits generated
     * code nor affects non-a010 scenes or non-stage objects.
     */
    if (target == 0x00DA45E8u &&
        (YZ_DIAG_ENABLED("YZ_STAGE_CLASSIFY_TRACE") ||
         g_yz_runtime_config.a010_stage_classify_repair)) {
        const uint32_t object_array = (uint32_t)ctx->gpr[4];
        const uint32_t object_index = (uint32_t)ctx->gpr[5];
        const uint64_t object_slot64 =
            (uint64_t)object_array + (uint64_t)object_index * 4u;
        const uint32_t object_slot =
            object_slot64 <= 0xFFFFFFFFu
                ? (uint32_t)object_slot64 : 0u;
        const uint32_t object =
            addr_readable(object_slot)
                ? vm_read32(object_slot) : 0u;
        if (yz_stage_render_instance(object)) {
            const uint16_t before = vm_read16(object + 0xB0u);
            const uint32_t gmd = vm_read32(object + 0x04u);

            void (*saved_trampoline)(void*) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            fn(ctx);
            yz_drain_trampolines(ctx);
            g_trampoline_fn = saved_trampoline;

            const uint16_t classified = vm_read16(object + 0xB0u);
            uint16_t repaired = classified;
            if (g_yz_runtime_config.a010_stage_classify_repair &&
                active_a010_root != 0u &&
                (classified & 0x0508u) == 0u) {
                repaired = (uint16_t)(classified | 0x0508u);
                vm_write16(object + 0xB0u, repaired);
            }

            if (YZ_DIAG_ENABLED("YZ_STAGE_CLASSIFY_TRACE")) {
                struct yz_stage_classify_record {
                    uint32_t gmd;
                    unsigned hits;
                };
                static yz_stage_classify_record records[64] = {};
                static unsigned record_count = 0;
                unsigned slot = record_count;
                for (unsigned i = 0; i < record_count; ++i) {
                    if (records[i].gmd == gmd) {
                        slot = i;
                        break;
                    }
                }
                if (slot == record_count && record_count < 64u)
                    records[record_count++] = {gmd, 0u};
                if (slot < 64u) {
                    yz_stage_classify_record& record = records[slot];
                    ++record.hits;
                    if (record.hits <= 4u ||
                        (record.hits & (record.hits - 1u)) == 0u) {
                        fprintf(stderr,
                                "[stage-classify] activeA010=%08X "
                                "object=%08X gmd=%08X hits=%u "
                                "b0=%04X->%04X->%04X "
                                "counts{104=%04X 108=%04X "
                                "110=%04X 112=%04X "
                                "138=%04X 13A=%04X "
                                "144=%04X 146=%04X "
                                "150=%04X 152=%04X "
                                "15C=%04X 15E=%04X} "
                                "ptrs{10C=%08X 114=%08X 128=%08X "
                                "12C=%04X 13C=%08X 148=%08X "
                                "154=%08X 160=%08X}\n",
                                active_a010_root,
                                object, gmd, record.hits,
                                before, classified, repaired,
                                (unsigned)vm_read16(object + 0x104u),
                                (unsigned)vm_read16(object + 0x108u),
                                (unsigned)vm_read16(object + 0x110u),
                                (unsigned)vm_read16(object + 0x112u),
                                (unsigned)vm_read16(object + 0x138u),
                                (unsigned)vm_read16(object + 0x13Au),
                                (unsigned)vm_read16(object + 0x144u),
                                (unsigned)vm_read16(object + 0x146u),
                                (unsigned)vm_read16(object + 0x150u),
                                (unsigned)vm_read16(object + 0x152u),
                                (unsigned)vm_read16(object + 0x15Cu),
                                (unsigned)vm_read16(object + 0x15Eu),
                                vm_read32(object + 0x10Cu),
                                vm_read32(object + 0x114u),
                                vm_read32(object + 0x128u),
                                (unsigned)vm_read16(object + 0x12Cu),
                                vm_read32(object + 0x13Cu),
                                vm_read32(object + 0x148u),
                                vm_read32(object + 0x154u),
                                vm_read32(object + 0x160u));
                        fflush(stderr);
                    }
                }
            }
            return;
        }
    }

    /*
     * Measure the static-stage render handoff itself.  These four virtual
     * methods are reached for the house and its sibling stage instances.  The
     * game publishes 0x20-byte geometry jobs through the TOC[-0x7410] list;
     * comparing its head before/after each synchronous virtual call tells us
     * whether the house diverges inside the method or only downstream.
     */
    if ((YZ_DIAG_ENABLED("YZ_STAGE_CALL_TRACE") ||
         YZ_DIAG_ENABLED("YZ_A010_STAGE_DMA_MAP") ||
         YZ_DIAG_ENABLED("YZ_A010_STAGE_READ_WATCH") ||
         YZ_DIAG_ENABLED("YZ_A010_CMD_ARENA_DUMP") ||
         YZ_DIAG_ENABLED("YZ_A010_STAGE_GROUP_TRACE") ||
         g_yz_runtime_config.a010_force_stage_meshes ||
         g_yz_runtime_config.a010_force_stage_main ||
         g_yz_runtime_config.a010_stage_group_wait ||
         YZ_DIAG_ENABLED("YZ_A010_STAGE_KIND1") ||
         YZ_DIAG_ENABLED("YZ_A010_ITEM_FLOW") ||
         YZ_DIAG_ENABLED("YZ_A010_PUBLISH_FLOW")) &&
        active_a010_root != 0u &&
        yz_stage_render_method(target) &&
        yz_stage_render_instance(dispatch_object)) {
        struct yz_stage_call_record {
            uint32_t target;
            uint32_t gmd;
            unsigned hits;
        };
        static yz_stage_call_record records[64] = {};
        static unsigned record_count = 0;
        const uint32_t gmd = vm_read32(dispatch_object + 0x04u);
        unsigned record_slot = record_count;
        for (unsigned i = 0; i < record_count; ++i) {
            if (records[i].target == target &&
                records[i].gmd == gmd) {
                record_slot = i;
                break;
            }
        }
        if (record_slot == record_count && record_count < 64u)
            records[record_count++] = {target, gmd, 0u};
        yz_stage_call_record* record =
            record_slot < 64u ? &records[record_slot] : nullptr;
        if (record)
            ++record->hits;

        /*
         * Diagnostic for the remaining incomplete orphanage scene.  Every
         * stage mesh carries a per-pass rejection mask at object+0x10.  The
         * live scene currently reaches the GPU with only the central
         * 189-mesh partition, while the sibling partitions retain 0x0001 /
         * 0x0010 rejection bits.  Clearing the masks immediately before each
         * shipped render method is a bounded A/B: if the healthy environment
         * reappears, the missing handoff is the CPU visibility update rather
         * than the geometry worker or GPU list linker.  Default-off until the
         * source-map run identifies the surviving list.
         */
        if (g_yz_runtime_config.a010_force_stage_meshes) {
            const uint32_t mesh_flags =
                vm_read32(dispatch_object + 0x10u);
            const uint32_t mesh_flags_end =
                vm_read32(dispatch_object + 0x0Cu);
            uint32_t mesh_count =
                mesh_flags_end >= mesh_flags
                    ? (mesh_flags_end - mesh_flags) / 2u : 0u;
            if (mesh_flags >= 0x40000000u &&
                mesh_flags <= 0x7FFFFE00u &&
                mesh_count <= 4096u) {
                for (uint32_t mesh_i = 0;
                     mesh_i < mesh_count; ++mesh_i)
                    vm_write16(mesh_flags + mesh_i * 2u, 0u);
            }
        }

        /*
         * The stage worker has populated both complete house mesh arrays,
         * but the shipped multipass renderer at FE5DB8 is disabled by the
         * missing 0x10 bit in object+B0. Without it, the worker's middle
         * material ranges never reach the common draw builder.
         *
         * Keep the repair restricted to the known a010 house GMD. No mesh
         * range, camera, or draw is fabricated; this only enables the shipped
         * renderer that consumes the already-authored worker output.
         */
        if (g_yz_runtime_config.a010_force_stage_main &&
            target == 0x00FE4508u &&
            gmd == 0x429D8A00u) {
            const uint16_t before_flags =
                vm_read16(dispatch_object + 0xB0u);
            if ((before_flags & 0x0010u) == 0u) {
                vm_write16(dispatch_object + 0xB0u,
                           (uint16_t)(before_flags | 0x0010u));
                static bool stage_main_repaired = false;
                if (!stage_main_repaired) {
                    stage_main_repaired = true;
                    g_yz_a010_stage_main_repaired = true;
                    const uint32_t table_fc =
                        vm_read32(dispatch_object + 0xFCu);
                    const uint32_t table_100 =
                        vm_read32(dispatch_object + 0x100u);
                    fprintf(stderr,
                            "[a010-stage-main] HOUSE flagsB0=%04X->%04X "
                            "boundary=FE4508-before-inlined-FE5DB8 "
                            "passCount=%u callPass=%u "
                            "FC=%08X{%08X,%08X,%08X,%08X} "
                            "100=%08X{%08X,%08X,%08X,%08X} "
                            "obj8=%08X\n",
                            (unsigned)before_flags,
                            (unsigned)(before_flags | 0x0010u),
                            (unsigned)vm_read8(
                                dispatch_object + 0xB2u),
                            (unsigned)ctx->gpr[4],
                            table_fc,
                            table_fc && addr_readable(table_fc + 3u)
                                ? vm_read32(table_fc) : 0u,
                            table_fc && addr_readable(table_fc + 7u)
                                ? vm_read32(table_fc + 4u) : 0u,
                            table_fc && addr_readable(table_fc + 11u)
                                ? vm_read32(table_fc + 8u) : 0u,
                            table_fc && addr_readable(table_fc + 15u)
                                ? vm_read32(table_fc + 12u) : 0u,
                            table_100,
                            table_100 && addr_readable(table_100 + 3u)
                                ? vm_read32(table_100) : 0u,
                            table_100 && addr_readable(table_100 + 7u)
                                ? vm_read32(table_100 + 4u) : 0u,
                            table_100 && addr_readable(table_100 + 11u)
                                ? vm_read32(table_100 + 8u) : 0u,
                            table_100 && addr_readable(table_100 + 15u)
                                ? vm_read32(table_100 + 12u) : 0u,
                            vm_read32(dispatch_object + 0x08u));
                    fflush(stderr);
                }
            }
        }

        if (record && record->hits == 1u) {
            const uint16_t object_flags =
                vm_read16(dispatch_object + 0xB0u);
            const uint32_t mesh_flags =
                vm_read32(dispatch_object + 0x10u);
            const uint32_t mesh_flags_end =
                vm_read32(dispatch_object + 0x0Cu);
            const uint32_t mesh_records =
                vm_read32(dispatch_object + 0xF4u);
            const uint32_t group_table =
                addr_readable(gmd + 0xF8u)
                    ? vm_read32(gmd + 0xF4u) : 0u;
            const uint32_t packed_counts =
                addr_readable(gmd + 0x2Cu)
                    ? vm_read32(gmd + 0x2Cu) : 0u;
            uint32_t mesh_count =
                mesh_flags_end >= mesh_flags
                    ? (mesh_flags_end - mesh_flags) / 2u : 0u;
            if (mesh_count > 4096u)
                mesh_count = 0u;
            uint32_t clear_0001 = 0u;
            uint32_t clear_0011 = 0u;
            uint32_t clear_0100 = 0u;
            uint32_t clear_2200 = 0u;
            uint32_t nonzero = 0u;
            uint16_t flags_or = 0u;
            if (mesh_flags >= 0x40000000u &&
                mesh_flags <= 0x7FFFFE00u) {
                for (uint32_t i = 0; i < mesh_count; ++i) {
                    const uint16_t flags =
                        vm_read16(mesh_flags + i * 2u);
                    if (flags)
                        ++nonzero;
                    flags_or = (uint16_t)(flags_or | flags);
                    if ((flags & 0x0001u) == 0u)
                        ++clear_0001;
                    if ((flags & 0x0011u) == 0u)
                        ++clear_0011;
                    if ((flags & 0x0100u) == 0u)
                        ++clear_0100;
                    if ((flags & 0x2200u) == 0u)
                        ++clear_2200;
                }
            }
            fprintf(stderr,
                    "[stage-filter] target=%08X object=%08X%s "
                    "flagsB0=%04X meshFlags=%08X "
                    "meshFlagsEnd=%08X meshRecords=%08X groupTable=%08X "
                    "packedCounts=%08X meshCount=%u nonzero=%u "
                    "flagsOR=%04X "
                    "eligible{0001=%u 0011=%u 0100=%u 2200=%u}\n",
                    target, dispatch_object,
                    dispatch_object == g_yz_stage_house_instance
                        ? " HOUSE" : "",
                    object_flags, mesh_flags, mesh_flags_end, mesh_records,
                    group_table, packed_counts, mesh_count, nonzero,
                    flags_or,
                    clear_0001, clear_0011,
                    clear_0100, clear_2200);
            if (mesh_flags >= 0x40000000u &&
                mesh_flags <= 0x7FFFFE00u) {
                fprintf(stderr,
                        "[stage-filter] object=%08X first16:"
                        " %04X %04X %04X %04X"
                        " %04X %04X %04X %04X"
                        " %04X %04X %04X %04X"
                        " %04X %04X %04X %04X\n",
                        dispatch_object,
                        vm_read16(mesh_flags + 0x00u),
                        vm_read16(mesh_flags + 0x02u),
                        vm_read16(mesh_flags + 0x04u),
                        vm_read16(mesh_flags + 0x06u),
                        vm_read16(mesh_flags + 0x08u),
                        vm_read16(mesh_flags + 0x0Au),
                        vm_read16(mesh_flags + 0x0Cu),
                        vm_read16(mesh_flags + 0x0Eu),
                        vm_read16(mesh_flags + 0x10u),
                        vm_read16(mesh_flags + 0x12u),
                        vm_read16(mesh_flags + 0x14u),
                        vm_read16(mesh_flags + 0x16u),
                        vm_read16(mesh_flags + 0x18u),
                        vm_read16(mesh_flags + 0x1Au),
                        vm_read16(mesh_flags + 0x1Cu),
                        vm_read16(mesh_flags + 0x1Eu));
            }
            fflush(stderr);
        }

        /*
         * The image-14 stage worker writes sixteen 0x10-byte render-group
         * descriptors to GMD+0xF4.  FE4508 consumes descriptors 0/1 and
         * FE8CA0 consumes descriptors 8/9.  Dump the completed house table
         * immediately before its first draw method so we can distinguish an
         * empty visibility result from a bad descriptor pointer without
         * tracing another whole frame.
         */
        if (YZ_DIAG_ENABLED("YZ_A010_STAGE_GROUP_TRACE") &&
            record && record->hits == 1u &&
            target == 0x00FE4508u &&
            gmd == 0x429D8A00u) {
            const uint32_t group_table =
                addr_readable(gmd + 0xF8u)
                    ? vm_read32(gmd + 0xF4u) : 0u;
            fprintf(stderr,
                    "[stage-groups] HOUSE object=%08X gmd=%08X "
                    "table=%08X meshRecords=%08X meshFlags=%08X\n",
                    dispatch_object, gmd, group_table,
                    vm_read32(dispatch_object + 0xF4u),
                    vm_read32(dispatch_object + 0x10u));
            if (addr_readable(group_table) &&
                addr_readable(group_table + 0xFFu)) {
                for (uint32_t offset = 0u;
                     offset < 0x100u; offset += 0x10u) {
                    const uint32_t descriptor =
                        group_table + offset;
                    const uint32_t count =
                        vm_read32(descriptor + 0x00u);
                    const uint32_t indices =
                        vm_read32(descriptor + 0x04u);
                    fprintf(stderr,
                            "[stage-groups] +%02X "
                            "%08X/%08X/%08X/%08X",
                            offset, count, indices,
                            vm_read32(descriptor + 0x08u),
                            vm_read32(descriptor + 0x0Cu));
                    if (count <= 4096u &&
                        addr_readable(indices) &&
                        addr_readable(indices +
                                      (count ? count * 2u - 1u : 0u))) {
                        const uint32_t shown =
                            count < 12u ? count : 12u;
                        fprintf(stderr, " indices=");
                        for (uint32_t i = 0u; i < shown; ++i)
                            fprintf(stderr, "%s%04X",
                                    i ? "/" : "",
                                    vm_read16(indices + i * 2u));
                        const uint32_t mesh_records =
                            vm_read32(dispatch_object + 0xF4u);
                        fprintf(stderr, " records=");
                        for (uint32_t i = 0u; i < shown; ++i) {
                            const uint32_t mesh_index =
                                vm_read16(indices + i * 2u);
                            const uint32_t mesh_record =
                                mesh_records + mesh_index * 12u;
                            fprintf(stderr,
                                    "%s%04X[%04X,%04X,%04X,%08X]",
                                    i ? "/" : "",
                                    mesh_index,
                                    (unsigned)vm_read16(
                                        mesh_record + 0x00u),
                                    (unsigned)vm_read16(
                                        mesh_record + 0x02u),
                                    (unsigned)vm_read16(
                                        mesh_record + 0x04u),
                                    vm_read32(mesh_record + 0x08u));
                        }
                    }
                    fputc('\n', stderr);
                }
            }
            fflush(stderr);
        }

        const uint32_t state =
            g_yz_game_toc
                ? vm_read32(g_yz_game_toc - 0x7410u) : 0u;
        const uint32_t before_head =
            addr_readable(state + 0x20u)
                ? vm_read32(state + 0x00u) : 0u;
        const uint32_t before_pending =
            addr_readable(state + 0x20u)
                ? vm_read32(state + 0x20u) : 0u;
        const uint32_t before_base =
            addr_readable(state + 0x20u)
                ? vm_read32(state + 0x08u) : 0u;
        /*
         * EA1E78 -> EABA74/EABABC is the common stage command builder.
         * It publishes one 0x20-byte command record by advancing the arena
         * stored at TOC[-0x72E8], not the later SPU list above.
         */
        const uint32_t command_arena =
            g_yz_game_toc
                ? vm_read32(g_yz_game_toc - 0x72E8u) : 0u;
        const bool command_arena_readable =
            addr_readable(command_arena + 0x20u);
        const uint32_t before_command_head =
            command_arena_readable
                ? vm_read32(command_arena + 0x00u) : 0u;
        const uint32_t before_command_end =
            command_arena_readable
                ? vm_read32(command_arena + 0x04u) : 0u;
        const uint32_t before_command_count =
            command_arena_readable
                ? vm_read32(command_arena + 0x1Cu) : 0u;

        /*
         * FE70A0 submits static-stage classification asynchronously, then the
         * shipped render sequence reaches FE4508.  On a010 the house result
         * currently lands after FE4508 has already observed an empty group
         * table, so the complete 157+21-entry building list is never consumed.
         *
         * Synchronize at that exact producer/consumer boundary.  This is
         * intentionally restricted to the known a010 house GMD and opt-in
         * while the handoff is validated; no camera, mesh, or draw data is
         * fabricated.  The worker still authors every list entry.
         */
        if (g_yz_runtime_config.a010_stage_group_wait &&
            target == 0x00FE4508u &&
            gmd == 0x429D8A00u) {
            const uint32_t group_table =
                addr_readable(gmd + 0xF8u)
                    ? vm_read32(gmd + 0xF4u) : 0u;
            if (addr_readable(group_table + 0x1Fu)) {
                const uint32_t before_group0 =
                    vm_read32(group_table + 0x00u);
                const uint32_t before_group1 =
                    vm_read32(group_table + 0x10u);
                const unsigned long long wait_started =
                    GetTickCount64();
                while (vm_read32(group_table + 0x00u) == 0u &&
                       yz_active_a010_root() != 0u &&
                       GetTickCount64() - wait_started < 15000u)
                    Sleep(1);
                const uint32_t after_group0 =
                    vm_read32(group_table + 0x00u);
                const uint32_t after_group1 =
                    vm_read32(group_table + 0x10u);
                static unsigned long group_wait_count = 0u;
                ++group_wait_count;
                if (group_wait_count <= 16u ||
                    (group_wait_count &
                     (group_wait_count - 1u)) == 0u) {
                    fprintf(stderr,
                            "[a010-stage-group-wait] n=%lu "
                            "table=%08X counts=%u/%u->%u/%u "
                            "wait=%llums\n",
                            group_wait_count, group_table,
                            before_group0, before_group1,
                            after_group0, after_group1,
                            GetTickCount64() - wait_started);
                    fflush(stderr);
                }
            }
        }

        /*
         * Arm the exact stage-record witness before entering FE70A0.  The
         * gs_task SPU can consume the freshly appended record before this
         * synchronous wrapper regains control, so publishing its expected EA
         * only after fn(ctx) leaves a real race in the diagnostic itself.
         */
        unsigned pending_stage_slot = 9u;
        if (record && record->hits == 1u &&
            target == 0x00FE70A0u) {
            /*
             * These nine calls are emitted in asset order.  Guest allocation
             * addresses moved as diagnostics changed timing, so the old GMD
             * switch silently lost one object.  Use the observed call order
             * as a fallback while retaining the address names for readable
             * traces.
             */
            static unsigned first_stage_order = 9u;
            if (gmd == 0x42AA7D00u)
                first_stage_order = 0u;
            switch (gmd) {
            case 0x42AA7D00u: pending_stage_slot = 0u; break;
            case 0x429D8A00u: pending_stage_slot = 1u; break;
            case 0x42A18A00u: pending_stage_slot = 2u; break;
            case 0x42A7E680u: pending_stage_slot = 3u; break;
            case 0x42A87300u:
            case 0x42A93C80u: pending_stage_slot = 4u; break;
            case 0x42A9C980u: pending_stage_slot = 5u; break;
            case 0x42A9D900u: pending_stage_slot = 6u; break;
            case 0x42AA3B80u: pending_stage_slot = 7u; break;
            case 0x42ABAA00u: pending_stage_slot = 8u; break;
            default: break;
            }
            if (pending_stage_slot >= 9u && first_stage_order < 9u)
                pending_stage_slot = first_stage_order;
            if (first_stage_order < 9u)
                ++first_stage_order;
            if (pending_stage_slot < 9u) {
                if (pending_stage_slot == 0u) {
                    for (unsigned si = 0; si < 9u; ++si) {
                        g_yz_a010_stage_record_ea[si] = 0u;
                        g_yz_a010_stage_payload_ea[si] = 0u;
                        g_yz_a010_stage_result_ea[si] = 0u;
                    }
                    _InterlockedExchange(
                        &g_yz_a010_stage_result_get_mask, 0);
                    _InterlockedIncrement(&g_yz_a010_stage_generation);
                }
                g_yz_a010_stage_record_ea[pending_stage_slot] =
                    before_command_head;
            }
        }

        /*
         * Publish the mature batch bounds before entering FE4508.  The gs_task
         * consumer may fetch this batch while fn(ctx) is still running; doing
         * this after the synchronous wrapper returns races the fetch and can
         * leave the focused downstream trace permanently unarmed.
         */
        if (target == 0x00FE4508u &&
            gmd == 0x42AA7D00u &&
            before_command_count >= 800u &&
            before_command_count <= 4096u) {
            const uint32_t batch_bytes =
                before_command_count * 0x20u;
            const uint32_t batch_start =
                before_command_head >= batch_bytes
                    ? before_command_head - batch_bytes : 0u;
            g_yz_a010_cmd_batch_start = batch_start;
            g_yz_a010_cmd_batch_end = before_command_head;
            g_yz_a010_cmd_batch_count = before_command_count;
            static bool batch_bounds_logged = false;
            if (!batch_bounds_logged) {
                batch_bounds_logged = true;
                fprintf(stderr,
                        "[a010-cmd-batch] count=%u range=%08X..%08X\n",
                        before_command_count, batch_start,
                        before_command_head);
                fflush(stderr);
            }
        }

        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        const unsigned long long started = GetTickCount64();
        const yz_a010_building_pass_scope saved_building_scope =
            g_yz_a010_building_pass_scope;
        if (YZ_DIAG_ENABLED("YZ_A010_HOUSE_MESH_TRACE") &&
            (target == 0x00FE4508u ||
             target == 0x00FE5DB8u) &&
            gmd == 0x429D8A00u) {
            g_yz_a010_building_pass_scope = {};
            g_yz_a010_building_pass_scope.target = target;
            g_yz_a010_building_pass_scope.stage_object = dispatch_object;
            g_yz_a010_building_pass_scope.command_arena = command_arena;
            g_yz_a010_building_pass_scope.active = true;
        }
        fn(ctx);
        yz_drain_trampolines(ctx);
        const unsigned long long elapsed =
            GetTickCount64() - started;
        const uint32_t after_head =
            addr_readable(state + 0x20u)
                ? vm_read32(state + 0x00u) : 0u;
        const uint32_t after_pending =
            addr_readable(state + 0x20u)
                ? vm_read32(state + 0x20u) : 0u;
        const uint32_t after_command_head =
            command_arena_readable
                ? vm_read32(command_arena + 0x00u) : 0u;
        const uint32_t after_command_end =
            command_arena_readable
                ? vm_read32(command_arena + 0x04u) : 0u;
        const uint32_t after_command_count =
            command_arena_readable
                ? vm_read32(command_arena + 0x1Cu) : 0u;
        if (g_yz_a010_building_pass_scope.active) {
            const auto& scope = g_yz_a010_building_pass_scope;
            fprintf(stderr,
                    "[a010-building-pass] target=%08X stage=%08X "
                    "builders=%u arena=%08X head=%08X->%08X "
                    "count=%u->%u first=%08X caller=%08X "
                    "args=%08X/%08X/%08X/%08X/%08X/%08X",
                    target, dispatch_object, scope.builder_calls,
                    command_arena, before_command_head,
                    after_command_head, before_command_count,
                    after_command_count, scope.first_record,
                    scope.first_caller,
                    scope.first_args[0], scope.first_args[1],
                    scope.first_args[2], scope.first_args[3],
                    scope.first_args[4], scope.first_args[5]);
            if (addr_readable(scope.first_record + 0x1Fu)) {
                fprintf(stderr, " record=");
                for (unsigned i = 0u; i < 8u; ++i)
                    fprintf(stderr, "%s%08X", i ? "/" : "",
                            vm_read32(scope.first_record + i * 4u));
            }
            fputc('\n', stderr);
            fflush(stderr);
        }
        g_yz_a010_building_pass_scope = saved_building_scope;
        g_trampoline_fn = saved_trampoline;

        /*
         * Dump exactly one complete, mature orphanage command-arena batch.
         * At the house's later FE4508 boundary the asynchronous geometry work
         * has expanded the nine top-level stage jobs into ~817 0x20-byte
         * records.  The final RSX stream contains only ~474 draws.  Capturing
         * the record words plus the first 0x80 bytes of each pointed payload
         * identifies which record class/objects vanish at the next consumer.
         * Bounded to one CSV per process and default-off.
         */
        if (YZ_DIAG_ENABLED("YZ_A010_CMD_ARENA_DUMP") &&
            target == 0x00FE4508u &&
            gmd == 0x42AA7D00u &&
            before_command_count >= 800u &&
            before_command_count <= 4096u) {
            static bool command_arena_dumped = false;
            if (!command_arena_dumped) {
                command_arena_dumped = true;
                const char* dump_path =
                    YZ_DIAG_VALUE("YZ_A010_CMD_ARENA_DUMP");
                FILE* dump =
                    dump_path && dump_path[0]
                        ? fopen(dump_path, "w") : nullptr;
                const uint32_t batch_bytes =
                    before_command_count * 0x20u;
                const uint32_t batch_start =
                    before_command_head >= batch_bytes
                        ? before_command_head - batch_bytes : 0u;
                if (dump && addr_readable(batch_start) &&
                    addr_readable(before_command_head - 1u)) {
                    fprintf(dump, "index,record");
                    for (unsigned word_i = 0; word_i < 8u; ++word_i)
                        fprintf(dump, ",r%u", word_i);
                    for (unsigned word_i = 0; word_i < 32u; ++word_i)
                        fprintf(dump, ",p%u", word_i);
                    fputc('\n', dump);
                    for (uint32_t record_i = 0;
                         record_i < before_command_count; ++record_i) {
                        const uint32_t record_ea =
                            batch_start + record_i * 0x20u;
                        const uint32_t payload =
                            vm_read32(record_ea + 0x04u);
                        fprintf(dump, "%u,0x%08X",
                                record_i, record_ea);
                        for (unsigned word_i = 0;
                             word_i < 8u; ++word_i)
                            fprintf(dump, ",%08X",
                                    vm_read32(record_ea +
                                              word_i * 4u));
                        for (unsigned word_i = 0;
                             word_i < 32u; ++word_i)
                            fprintf(dump, ",%08X",
                                    addr_readable(payload +
                                                  word_i * 4u)
                                        ? vm_read32(payload +
                                                    word_i * 4u)
                                        : 0xDEADDEADu);
                        fputc('\n', dump);
                    }
                    fclose(dump);
                    fprintf(stderr,
                            "[a010-cmd-arena] dumped %u records "
                            "range=%08X..%08X to %s\n",
                            before_command_count, batch_start,
                            before_command_head, dump_path);
                } else {
                    if (dump)
                        fclose(dump);
                    fprintf(stderr,
                            "[a010-cmd-arena] dump failed count=%u "
                            "range=%08X..%08X path=%s\n",
                            before_command_count, batch_start,
                            before_command_head,
                            dump_path ? dump_path : "<empty>");
                }
                fflush(stderr);
            }
        }

        /*
         * Preserve the exact 0x20-byte top-level stage job associated with
         * each object.  The SPU launch-map probe can then search for these
         * words in the launch descriptor/input and identify the first object
         * that disappears between FE70A0 and the geometry worker.
         */
        if (record && record->hits == 1u &&
            target == 0x00FE70A0u &&
            after_command_head == before_command_head + 0x20u &&
            addr_readable(before_command_head + 0x1Fu)) {
            if (pending_stage_slot < 9u) {
                g_yz_a010_stage_payload_ea[pending_stage_slot] =
                    vm_read32(before_command_head + 0x04u);
                if (pending_stage_slot == 8u &&
                    YZ_DIAG_ENABLED("YZ_A010_STAGE_READ_WATCH")) {
                    static bool stage_read_watch_armed = false;
                    if (!stage_read_watch_armed &&
                        g_yz_a010_stage_record_ea[0] != 0u) {
                        stage_read_watch_armed = true;
                        yz_watch_arm_read(
                            g_yz_a010_stage_record_ea[0]);
                    }
                }
            }
            fprintf(stderr,
                    "[stage-job-record] object=%08X gmd=%08X ea=%08X "
                    "words=%08X/%08X/%08X/%08X/"
                    "%08X/%08X/%08X/%08X\n",
                    dispatch_object, gmd, before_command_head,
                    vm_read32(before_command_head + 0x00u),
                    vm_read32(before_command_head + 0x04u),
                    vm_read32(before_command_head + 0x08u),
                    vm_read32(before_command_head + 0x0Cu),
                    vm_read32(before_command_head + 0x10u),
                    vm_read32(before_command_head + 0x14u),
                    vm_read32(before_command_head + 0x18u),
                    vm_read32(before_command_head + 0x1Cu));
            fflush(stderr);
        }

        if (record &&
            (record->hits <= 4u ||
             (record->hits & (record->hits - 1u)) == 0u)) {
            const int32_t job_bytes =
                (int32_t)(after_head - before_head);
            fprintf(stderr,
                    "[stage-call] target=%08X lr=%08X hits=%u "
                    "object=%08X gmd=%08X%s mask=%08X "
                    "state=%08X base=%08X "
                    "head=%08X->%08X jobBytes=%d jobs=%d "
                    "pending=%08X->%08X "
                    "cmdArena=%08X cmdHead=%08X->%08X "
                    "cmdEnd=%08X->%08X cmdCount=%u->%u "
                    "cmdBytes=%d cmdRecords=%d elapsed=%llums\n",
                    target, (uint32_t)ctx->lr, record->hits,
                    dispatch_object, gmd,
                    dispatch_object == g_yz_stage_house_instance
                        ? " HOUSE" : "",
                    vm_read32(dispatch_object + 0x08u),
                    state, before_base,
                    before_head, after_head, job_bytes,
                    job_bytes / 0x20,
                    before_pending, after_pending,
                    command_arena,
                    before_command_head, after_command_head,
                    before_command_end, after_command_end,
                    before_command_count, after_command_count,
                    (int32_t)(after_command_head -
                              before_command_head),
                    (int32_t)(after_command_head -
                              before_command_head) / 0x20,
                    elapsed);
            fflush(stderr);
        }
        return;
    }

    if (stage_gmd_allocator) {
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        yz_stage_gmd_allocator_return(stage_gmd_allocator, ctx);
        g_trampoline_fn = saved_trampoline;
        return;
    }

#if defined(YZ_PPU_SAMPLE)
    static const int sample_capture_enabled =
        std::getenv("YZ_PPU_SAMPLE_RUN") != nullptr;
    static const int sample_present_target =
        std::getenv("YZ_PPU_SAMPLE_PRESENT") != nullptr;
    static const uint32_t sample_window_frames = []() {
        const char* value = std::getenv("YZ_PPU_SAMPLE_FRAMES");
        if (!value) return 4u;
        const unsigned long parsed = std::strtoul(value, nullptr, 0);
        return (uint32_t)(parsed < 4u ? 4u :
                          parsed > 600u ? 600u : parsed);
    }();
    if (sample_capture_enabled && !sample_present_target &&
        resolved_code == 0x000195C0u &&
        ((uint32_t)ctx->lr == 0x00C961C0u ||
         (uint32_t)ctx->lr == 0x00C96204u)) {
        static int sample_window_started = 0;
        static int sample_window_done = 0;
        static uint32_t sample_window_start_frame = 0;
        const uint32_t frame = rsx_live_draw_get_frames();
        if (!sample_window_started && frame >= 1578u) {
            sample_window_started = 1;
            sample_window_start_frame = frame;
            yz_ppu_perf_window_begin(frame);
        } else if (sample_window_started && !sample_window_done &&
                   frame >= sample_window_start_frame +
                                sample_window_frames) {
            yz_ppu_perf_window_dump(frame);
            sample_window_done = 1;
        }
    }
#endif

#if defined(YZ_PERF_PROFILE)
    /*
     * The orphanage transition watchdog caught the main PPU thread inside
     * func_00C96170's selector-13 scene walker, specifically its indirect
     * func_000195C0 node callback.  Execute only that callback synchronously
     * in the symbolized profile lane so its complete tail-call chain can be
     * timed without touching generated recompilation output.  Release/fast
     * builds retain the ordinary trampoline hand-off below.
     */
    if (resolved_code == 0x000195C0u &&
        ((uint32_t)ctx->lr == 0x00C961C0u ||
         (uint32_t)ctx->lr == 0x00C96204u)) {
        struct yz_scene_callback_frame_profile {
            uint32_t frame;
            uint64_t calls;
            uint64_t elapsed_us;
            uint64_t max_us;
            uint32_t max_node;
            uint32_t max_selector;
        };
        static yz_scene_callback_frame_profile aggregate = {};
        static uint32_t spu_window_start = 0u;
        static int spu_window_done = 0;
        const uint32_t frame = rsx_live_draw_get_frames();
        if (spu_window_start == 0u) {
            spu_window_start = frame;
            spu_perf_window_begin(frame);
        } else if (!spu_window_done && frame >= spu_window_start + 70u) {
            spu_perf_window_dump(frame);
            spu_window_done = 1;
        }
        if (aggregate.calls != 0u && aggregate.frame != frame) {
            fprintf(stderr,
                    "[scene-callback-perf] frame=%u calls=%llu "
                    "total=%.3fms max=%.3fms node=%08X selector=%u\n",
                    aggregate.frame,
                    (unsigned long long)aggregate.calls,
                    (double)aggregate.elapsed_us / 1000.0,
                    (double)aggregate.max_us / 1000.0,
                    aggregate.max_node, aggregate.max_selector);
            aggregate = {};
        }
        if (aggregate.calls == 0u)
            aggregate.frame = frame;

        const uint32_t node = (uint32_t)ctx->gpr[3];
        const uint32_t selector = (uint32_t)ctx->gpr[4];
        const auto started = std::chrono::steady_clock::now();
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        g_trampoline_fn = saved_trampoline;
        const uint64_t elapsed_us = (uint64_t)
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();

        ++aggregate.calls;
        aggregate.elapsed_us += elapsed_us;
        if (elapsed_us > aggregate.max_us) {
            aggregate.max_us = elapsed_us;
            aggregate.max_node = node;
            aggregate.max_selector = selector;
        }
        if (elapsed_us >= 20000u) {
            fprintf(stderr,
                    "[scene-callback-slow] frame=%u elapsed=%.3fms "
                    "node=%08X selector=%u lr=%08X\n",
                    frame, (double)elapsed_us / 1000.0,
                    node, selector, (uint32_t)ctx->lr);
            fflush(stderr);
        }
        return;
    }
#endif

#if defined(YZ_PPU_SAMPLE)
    /* func_00C70D90 is the mt_task_thread callback that owns the long
     * orphanage frames.  It is only a pair of guest work-list walkers: these
     * two indirect call sites execute the individual entries.  Time each
     * entry in the profile build so a genuinely slow guest callback can be
     * distinguished from host scheduling delay around the outer walker. */
    if (sample_capture_enabled && yz_thread_current_id() == 4u &&
        ((uint32_t)ctx->lr == 0x00C70E38u ||
         (uint32_t)ctx->lr == 0x00C70EB8u)) {
        const uint32_t entry_lr = (uint32_t)ctx->lr;
        const uint32_t entry_r3 = (uint32_t)ctx->gpr[3];
        const uint32_t entry_r4 = (uint32_t)ctx->gpr[4];
        const auto started = std::chrono::steady_clock::now();
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        g_trampoline_fn = saved_trampoline;
        const uint64_t elapsed_us = (uint64_t)
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
        if (g_yz_mt_outer_active) {
            ++g_yz_mt_entry_calls;
            g_yz_mt_entry_elapsed_us += elapsed_us;
            unsigned slot = g_yz_mt_entry_count;
            for (unsigned i = 0; i < g_yz_mt_entry_count; ++i) {
                if (g_yz_mt_entries[i].target == resolved_code) {
                    slot = i;
                    break;
                }
            }
            if (slot == g_yz_mt_entry_count &&
                g_yz_mt_entry_count < 64u) {
                g_yz_mt_entries[g_yz_mt_entry_count++] =
                    {resolved_code, 0u, 0u};
            }
            if (slot < 64u) {
                ++g_yz_mt_entries[slot].calls;
                g_yz_mt_entries[slot].elapsed_us += elapsed_us;
            }
        }
        if (elapsed_us >= 5000u) {
            fprintf(stderr,
                    "[mt-task-entry] frame=%u target=%08X lr=%08X "
                    "elapsed=%.3fms r3=%08X r4=%08X\n",
                    rsx_live_draw_get_frames(), resolved_code, entry_lr,
                    (double)elapsed_us / 1000.0, entry_r3, entry_r4);
            fflush(stderr);
        }
        return;
    }

    /* mt_task_thread (guest tid 4) executes queued callbacks at these two
     * dispatcher call sites and posts semaphore 4 only after a callback
     * returns.  The main-thread profile proved every 300-400 ms orphanage
     * stall ends exactly when tid 4 posts.  Execute the callback synchronously
     * in this profile-only lane so its complete trampoline tail can be timed
     * and the responsible guest target can be named. */
    if (sample_capture_enabled && yz_thread_current_id() == 4u &&
        resolved_code == 0x00C70D90u &&
        ((uint32_t)ctx->lr == 0x00E5D444u ||
         (uint32_t)ctx->lr == 0x00E5D470u)) {
        const uint32_t callback_lr = (uint32_t)ctx->lr;
        const uint32_t callback_r3 = (uint32_t)ctx->gpr[3];
        const uint32_t callback_r4 = (uint32_t)ctx->gpr[4];
        const uint32_t first_count = addr_readable(callback_r3 + 0x90Cu)
            ? vm_read32(callback_r3 + 0x908u) : 0u;
        const uint32_t second_count = addr_readable(callback_r3 + 0x104u)
            ? vm_read32(callback_r3 + 0x100u) : 0u;
        const uint32_t callback_start_frame = rsx_live_draw_get_frames();
        yz_filetime creation = {}, exit = {};
        yz_filetime kernel_before = {}, user_before = {};
        yz_filetime kernel_after = {}, user_after = {};
        const int have_cpu_before = GetThreadTimes(
            GetCurrentThread(), &creation, &exit,
            &kernel_before, &user_before);
        g_yz_mt_entry_calls = 0u;
        g_yz_mt_entry_elapsed_us = 0u;
        g_yz_mt_entry_count = 0u;
        memset(g_yz_mt_entries, 0, sizeof(g_yz_mt_entries));
        g_yz_mt_outer_active = true;
        const auto started = std::chrono::steady_clock::now();
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        g_yz_mt_outer_active = false;
        g_trampoline_fn = saved_trampoline;
        const uint64_t elapsed_us = (uint64_t)
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
        const int have_cpu_after = GetThreadTimes(
            GetCurrentThread(), &creation, &exit,
            &kernel_after, &user_after);
        uint64_t cpu_us = 0u;
        if (have_cpu_before && have_cpu_after) {
            const uint64_t before = yz_filetime_ticks(kernel_before) +
                                    yz_filetime_ticks(user_before);
            const uint64_t after = yz_filetime_ticks(kernel_after) +
                                   yz_filetime_ticks(user_after);
            if (after >= before) cpu_us = (after - before) / 10u;
        }
        if (elapsed_us >= 5000u) {
            const yz_mt_entry_aggregate* hot_entry = nullptr;
            for (unsigned i = 0; i < g_yz_mt_entry_count; ++i) {
                if (!hot_entry || g_yz_mt_entries[i].elapsed_us >
                                      hot_entry->elapsed_us)
                    hot_entry = &g_yz_mt_entries[i];
            }
            fprintf(stderr,
                    "[mt-task-callback] frame=%u->%u target=%08X lr=%08X "
                    "wall=%.3fms cpu=%.3fms cpu_pct=%.1f "
                    "list=%u/%u entries=%u/%.3fms "
                    "hot=%08X/%u/%.3fms r3=%08X r4=%08X\n",
                    callback_start_frame, rsx_live_draw_get_frames(),
                    resolved_code, callback_lr,
                    (double)elapsed_us / 1000.0,
                    (double)cpu_us / 1000.0,
                    elapsed_us ? 100.0 * (double)cpu_us /
                                     (double)elapsed_us : 0.0,
                    first_count, second_count, g_yz_mt_entry_calls,
                    (double)g_yz_mt_entry_elapsed_us / 1000.0,
                    hot_entry ? hot_entry->target : 0u,
                    hot_entry ? hot_entry->calls : 0u,
                    hot_entry ? (double)hot_entry->elapsed_us / 1000.0 : 0.0,
                    callback_r3, callback_r4);
            fflush(stderr);
        }
        return;
    }
#endif

    g_trampoline_fn = (void (*)(void*))fn;
}

extern "C" void yz_call_guest_opd(uint32_t opd_addr, ppu_context* ctx)
{
    if (!addr_readable(opd_addr)) {
        fprintf(stderr, "[dispatch] guest callback OPD 0x%08X out of range\n", opd_addr);
        return;
    }
    uint32_t code = vm_read32(opd_addr);
    uint32_t toc  = vm_read32(opd_addr + 4);
    yz_ppu_fn fn = yz_lookup_func(code);
    if (!fn) {
        fprintf(stderr, "[dispatch] guest callback code 0x%08X (opd 0x%08X) not lifted\n",
                code, opd_addr);
        return;
    }
    uint64_t saved_toc = ctx->gpr[2];
    ctx->gpr[2] = toc;
    if (code == 0x00E7DB10u) {
        const long cause = (long)(uint32_t)ctx->gpr[3];
        const long epoch = yz_ucmd_handler_begin(cause);
        void (*saved_trampoline)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        fn(ctx);
        yz_drain_trampolines(ctx);
        yz_ucmd_handler_end(cause, epoch);
        g_trampoline_fn = saved_trampoline;
    } else {
        fn(ctx);
        yz_drain_trampolines(ctx);
    }
    ctx->gpr[2] = saved_toc;
}
