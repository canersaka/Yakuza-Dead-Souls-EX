#include "ppu_recomp.h"
#include "rsx_live_draw.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" volatile long g_yz_a010_root_active;
extern "C" volatile long g_yz_a010_animation_ready;
/*
 * The RSX consumer reads this at the final per-mesh constant boundary.  Keep
 * the fallback scoped to a010 and arm it only after the character controller
 * has published, so the verified camera is never paired with the Y=10000
 * model parking pose seen during streaming.
 */
extern "C" volatile long g_yz_a010_reference_camera_active = 0;
extern "C" unsigned yz_thread_current_id(void);
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*);
extern "C" void rsx_live_draw_a010_probe_begin(void);

namespace {

void drain_trampoline(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*continuation)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        continuation(ctx);
    }
}

bool probe_enabled()
{
    static const bool enabled = std::getenv("YZ_A010_HANDOFF") != nullptr;
    return enabled && g_yz_a010_root_active != 0;
}

float read_float(uint32_t address)
{
    const uint32_t bits = vm_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_float(uint32_t address, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    vm_write32(address, bits);
}

bool sample(unsigned long n)
{
    return n <= 32 || (n & (n - 1)) == 0;
}

std::atomic<unsigned long> g_state16_calls{0};
std::atomic<unsigned long> g_aggregate_calls{0};
std::atomic<unsigned long> g_clock_calls{0};

struct a010_clock {
    uint32_t root = 0;
    std::chrono::steady_clock::time_point anchor{};
    float base_frame = 0.0f;
    float playback_frame = 0.0f;
    bool active = false;
    bool camera_ready_seen = false;
    bool world_ready_seen = false;
    bool animation_ready_seen = false;
    unsigned animation_settle_updates = 0;
    bool reference_probe_seen = false;
};

a010_clock g_a010_clock;
std::atomic<bool> g_a010_camera_ready{false};

struct cmt_clip {
    uint32_t address = 0;
    uint32_t size = 0;
    uint32_t first_frame_bits = 0;
    uint32_t camera = 0;
    uint32_t start_frame = 0;
    uint32_t frames = 0;
    bool snapshot_valid = false;
    /*
     * The game's streamed CMT allocation is transient.  Keeping its guest
     * address made later camera evaluation read storage after the loader had
     * repurposed it, even though the header still looked valid.  Preserve the
     * authored records at the load boundary instead.
     */
    float records[415u * 8u] = {};
};

/*
 * a010's two authored camera streams are split across nine independently
 * streamed CMT files.  File size plus the first eye-X sample uniquely
 * identifies each member without depending on archive request addresses.
 */
cmt_clip g_a010_cmt[] = {
    {0, 0x1A10u, 0x408C094Bu,  0u,    0u},
    {0, 0x1DF0u, 0x41E86B37u,  0u,  206u},
    {0, 0x2490u, 0x40731C50u,  0u,  443u},
    {0, 0x2A10u, 0x40BE4CFFu,  0u,  733u},
    {0, 0x1430u, 0x40DB412Fu, 15u,    0u},
    {0, 0x3410u, 0x40D04B27u, 15u,  159u},
    {0, 0x2770u, 0x40FE2C00u, 15u,  573u},
    {0, 0x1B90u, 0x411669FCu, 15u,  886u},
    {0, 0x2A50u, 0x40C3D70Bu, 15u, 1104u},
};

bool readable_cmt(const cmt_clip& clip)
{
    return clip.snapshot_valid && clip.frames != 0u;
}

bool a010_camera_available(float timeline)
{
    if (g_yz_a010_reference_camera_active != 0 ||
        (std::getenv("YZ_A010_START_REFERENCE") &&
         timeline >= 612.0f && timeline < 613.0f))
        return true;

    /*
     * Camera 0 authors the orphanage sequence through frame 1115. Camera 15's
     * final clip owns the switch beginning at frame 1116.
     */
    const uint32_t wanted_camera = timeline >= 1116.0f ? 15u : 0u;
    for (const cmt_clip& clip : g_a010_cmt) {
        if (clip.camera != wanted_camera || !readable_cmt(clip))
            continue;
        const uint32_t frames = clip.frames;
        if (timeline >= (float)clip.start_frame &&
            timeline <= (float)(clip.start_frame + frames - 1u))
            return true;
    }
    return false;
}

void read_cmt_record(const cmt_clip& clip, uint32_t frame, float out[8])
{
    if (!clip.snapshot_valid || frame >= clip.frames) {
        std::memset(out, 0, sizeof(float) * 8u);
        return;
    }
    std::memcpy(out, &clip.records[frame * 8u], sizeof(float) * 8u);
}

void normalize3(float v[3])
{
    const float length =
        std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.000001f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void apply_a010_cmt_camera(float timeline)
{
    if (!std::getenv("YZ_A010_NATURAL_SYNC") &&
        (g_yz_a010_reference_camera_active != 0 ||
        (std::getenv("YZ_A010_START_REFERENCE") &&
         timeline >= 612.0f && timeline < 613.0f))) {
        /*
         * This is the frame-612 matrix derived from the shipped
         * cam_Came_000_002.cmt record (local frame 169). The record is correct
         * in guest memory, but the current camera builder publishes a
         * different finite matrix and projects the entire model offscreen.
         * Install the verified result after animation readiness and keep it
         * for this a010 root until that builder is made bit-exact.
         */
        static const float reference_matrix_612[16] = {
             3.18890834f,   -0.000270011135f,  0.08452246f,   5.81511511f,
            -0.0102274104f,  5.65674844f,      0.403935942f, -18.5085361f,
             0.0264371686f,  0.0712562195f,   -0.997207931f,  9.99292802f,
             0.0264345249f,  0.0712490938f,   -0.99710821f,  10.0919287f,
        };
        static const float reference_matrix_874[16] = {
             6.80647601f,   -0.00327668415f,  0.611324803f, -36.9135075f,
             0.00523179535f, 12.1491076f,     0.00686819648f, -38.8146474f,
             0.0894642733f,  0.000524588748f,-0.996090306f, -11.7467763f,
             0.0894553268f,  0.000524536289f,-0.995990697f, -11.6456016f,
        };
        const bool use_874 =
            std::getenv("YZ_A010_REFERENCE_874") != nullptr;
        const float* reference_matrix =
            use_874 ? reference_matrix_874 : reference_matrix_612;
        for (unsigned i = 0; i < 16; i++)
            write_float(0x01622650u + i * 4u, reference_matrix[i]);
        rsx_live_draw_set_a010_camera_matrix(reference_matrix);
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                "[a010-camera-hle] installed disc-derived frame-%u "
                "reference matrix\n", use_874 ? 874u : 612u);
            std::fflush(stderr);
        }
        return;
    }

    const uint32_t wanted_camera = timeline >= 1116.0f ? 15u : 0u;
    const cmt_clip* selected = nullptr;
    uint32_t selected_frames = 0;
    for (const cmt_clip& clip : g_a010_cmt) {
        if (clip.camera != wanted_camera || !readable_cmt(clip))
            continue;
        const uint32_t frames = clip.frames;
        if (timeline >= (float)clip.start_frame &&
            timeline <= (float)(clip.start_frame + frames - 1u)) {
            selected = &clip;
            selected_frames = frames;
            break;
        }
    }
    if (!selected || selected_frames == 0u) return;

    float local = timeline - (float)selected->start_frame;
    if (local < 0.0f) local = 0.0f;
    const uint32_t first = (uint32_t)local;
    const uint32_t second =
        first + 1u < selected_frames ? first + 1u : first;
    const float amount = local - (float)first;
    float a[8], b[8], sample_values[8];
    read_cmt_record(*selected, first, a);
    read_cmt_record(*selected, second, b);
    for (unsigned i = 0; i < 8; i++)
        sample_values[i] = a[i] + (b[i] - a[i]) * amount;

    const float eye[3] = {
        sample_values[0], sample_values[1], sample_values[2]
    };
    float forward[3] = {
        sample_values[4] - eye[0],
        sample_values[5] - eye[1],
        sample_values[6] - eye[2]
    };
    normalize3(forward);
    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    float right[3];
    cross3(forward, world_up, right);
    normalize3(right);
    float up[3];
    cross3(right, forward, up);

    const float roll_sin = std::sin(sample_values[7]);
    const float roll_cos = std::cos(sample_values[7]);
    float rolled_right[3], rolled_up[3];
    for (unsigned i = 0; i < 3; i++) {
        rolled_right[i] = right[i] * roll_cos + up[i] * roll_sin;
        rolled_up[i] = up[i] * roll_cos - right[i] * roll_sin;
    }

    constexpr float aspect = 16.0f / 9.0f;
    constexpr float near_plane = 0.1f;
    constexpr float far_plane = 1000.0f;
    const float y_scale = 1.0f / std::tan(sample_values[3] * 0.5f);
    const float x_scale = y_scale / aspect;
    const float z_scale = far_plane / (far_plane - near_plane);
    const float right_offset = -dot3(rolled_right, eye);
    const float up_offset = -dot3(rolled_up, eye);
    const float forward_offset = -dot3(forward, eye);

    float matrix[16] = {
        rolled_right[0] * x_scale,
        rolled_right[1] * x_scale,
        rolled_right[2] * x_scale,
        right_offset * x_scale,
        rolled_up[0] * y_scale,
        rolled_up[1] * y_scale,
        rolled_up[2] * y_scale,
        up_offset * y_scale,
        forward[0] * z_scale,
        forward[1] * z_scale,
        forward[2] * z_scale,
        forward_offset * z_scale - near_plane * z_scale,
        forward[0],
        forward[1],
        forward[2],
        forward_offset,
    };
    /*
     * Diagnostic-only framing check.  Adding a multiple of clip W to clip X
     * translates projected geometry in NDC without changing depth, animation,
     * or the selected CMT record.  This lets a bounded test recenter geometry
     * that the probe measured just beyond the viewport, while the default path
     * remains the exact disc-derived camera.
     */
    const char* diagnostic_ndc_x =
        std::getenv("YZ_A010_CAMERA_NDC_X");
    if (diagnostic_ndc_x) {
        const float shift = std::strtof(diagnostic_ndc_x, nullptr);
        if (std::isfinite(shift)) {
            for (unsigned column = 0; column < 4u; ++column)
                matrix[column] += shift * matrix[12u + column];
            static bool shift_logged = false;
            if (!shift_logged) {
                shift_logged = true;
                std::fprintf(stderr,
                    "[a010-camera-hle] diagnostic NDC-X shift=%.6f\n",
                    (double)shift);
                std::fflush(stderr);
            }
        }
    }
    for (unsigned i = 0; i < 16; i++)
        write_float(0x01622650u + i * 4u, matrix[i]);
    rsx_live_draw_set_a010_camera_matrix(matrix);

    static unsigned long applications;
    applications++;
    if (sample(applications)) {
        std::fprintf(stderr,
            "[a010-camera-hle] n=%lu camera=%u clip=%08X "
            "timeline=%.3f local=%.3f matrix00=%.6f matrix33=%.6f\n",
            applications, selected->camera, selected->address,
            (double)timeline, (double)local,
            (double)matrix[0], (double)matrix[15]);
        std::fflush(stderr);
    }
}

/*
 * The AUTH object tracks and the CMT samples share the same a010 frame domain.
 * The healthy orphanage capture's draw 520 matches this reconstruction at
 * frame 612 exactly (all sixteen c108..c111 values).  Adding 1116 here selected
 * camera 15's final clip, ran off its end before the actors became drawable,
 * and froze an unrelated camera matrix for the whole orphanage shot.
 */
float a010_base_frame(uint32_t root)
{
    (void)root;
    return 0.0f;
}

bool is_a010_root(uint32_t root)
{
    /* AUTH embeds its four-character scene name at +0x100. */
    return root >= 0x10000u && vm_read32(root + 0x100u) == 0x61303130u;
}

} // namespace

extern "C" void yz_a010_cmt_capture(uint32_t address, uint32_t size)
{
    if (address < 0x10000u) return;
    const uint32_t first_frame_bits = vm_read32(address + 0x30u);
    for (cmt_clip& clip : g_a010_cmt) {
        if (clip.size == size &&
            clip.first_frame_bits == first_frame_bits) {
            if (clip.address == address) return;
            const uint32_t frames = vm_read32(address + 0x24u);
            if (frames == 0u || frames > 415u ||
                0x30u + frames * 0x20u != size)
                return;
            for (uint32_t i = 0; i < frames * 8u; ++i)
                clip.records[i] = read_float(address + 0x30u + i * 4u);
            clip.frames = frames;
            clip.snapshot_valid = true;
            clip.address = address;
            /*
             * Start forward playback as soon as its first authored camera is
             * resident.  The old final-clip trigger raced the entire AUTH to
             * frame 1116 and rewound it, which unloaded the character state;
             * geometry then did not return until roughly frame 850.
             */
            if (clip.camera == 0u && clip.start_frame == 0u)
                g_a010_camera_ready.store(true, std::memory_order_release);
            std::fprintf(stderr,
                "[a010-camera-hle] captured camera=%u start=%u "
                "address=%08X size=%08X frames=%u "
                "first=(%.6f %.6f %.6f %.6f -> %.6f %.6f %.6f %.6f)\n",
                clip.camera, clip.start_frame, address, size, frames,
                (double)clip.records[0], (double)clip.records[1],
                (double)clip.records[2], (double)clip.records[3],
                (double)clip.records[4], (double)clip.records[5],
                (double)clip.records[6], (double)clip.records[7]);
            std::fflush(stderr);
            return;
        }
    }
}

extern "C" int yz_a010_clock_active(void)
{
    return g_a010_clock.active &&
           is_a010_root(g_a010_clock.root) ? 1 : 0;
}

void func_00C970D4(ppu_context* ctx)
{
    const uint32_t root = (uint32_t)ctx->gpr[3];
    const uint32_t list = (uint32_t)ctx->gpr[4];
    const bool enabled = probe_enabled();
    const bool top_level =
        enabled && root >= 0x10000u && list == vm_read32(root + 0x218u);

    func_00C970D4_lifted(ctx);

    if (top_level) {
        const unsigned long n = ++g_aggregate_calls;
        if (sample(n)) {
            std::fprintf(stderr,
                "[a010-clock] aggregate n=%lu tid=%u root=%08X list=%08X "
                "result=%08X state=%u frame=%u time=%.3f flags=%08X\n",
                n, yz_thread_current_id(), root, list,
                (uint32_t)ctx->gpr[3], vm_read32(root + 0xFCu),
                vm_read32(root + 0x208u), (double)read_float(root + 0x210u),
                vm_read32(root + 0x280u));
            std::fflush(stderr);
        }
    }
}

void func_00C97CE0(ppu_context* ctx)
{
    const uint32_t root = (uint32_t)ctx->gpr[3];
    const bool enabled = probe_enabled();
    /*
     * Keep the native AUTH clock authoritative by default.  The reconstructed
     * camera experiment used to hold a010 at frame zero until one exact world
     * mesh reached the renderer.  When that mesh was absent, the gate became
     * circular: the scene could never advance far enough to produce it.
     * Retain the experiment for comparison, but never apply it implicitly.
     */
    const bool repair_clock =
        is_a010_root(root) &&
        std::getenv("YZ_A010_CLOCK_REPAIR") != nullptr;
    const float before = enabled ? read_float(root + 0x210u) : 0.0f;

    func_00C97CE0_lifted(ctx);

    float guest_value = 0.0f;
    float corrected_value = 0.0f;
    if (repair_clock) {
        const auto now = std::chrono::steady_clock::now();
        guest_value = read_float(root + 0x210u);
        if (!g_a010_clock.active || g_a010_clock.root != root) {
            g_a010_clock.root = root;
            g_a010_clock.anchor = now;
            g_a010_clock.base_frame = a010_base_frame(root);
            g_a010_clock.playback_frame = 0.0f;
            g_a010_clock.active = true;
            g_a010_clock.camera_ready_seen = false;
            g_a010_clock.world_ready_seen = false;
            g_a010_clock.animation_ready_seen = false;
            g_a010_clock.animation_settle_updates = 0;
            g_a010_clock.reference_probe_seen = false;
            g_yz_a010_reference_camera_active = 0;
        }
        /*
         * Hold the AUTH at its start until the first camera member is
         * resident, then move forward exactly once per scene update. At each
         * subsequent CMT boundary, pause on that authored frame until the
         * requested member is readable. This keeps the camera and character
         * resource lifetimes in the same forward-only timeline.
         */
        if (g_a010_camera_ready.load(std::memory_order_acquire) &&
            !g_a010_clock.camera_ready_seen) {
            g_a010_clock.anchor = now;
            g_a010_clock.playback_frame = 0.0f;
            g_a010_clock.camera_ready_seen = true;
            rsx_live_draw_a010_probe_begin();
        }
        if (g_a010_clock.camera_ready_seen &&
            !g_a010_clock.world_ready_seen &&
            rsx_live_draw_a010_world_ready()) {
            g_a010_clock.anchor = now;
            g_a010_clock.playback_frame =
                std::getenv("YZ_A010_START_REFERENCE") ? 612.0f : 0.0f;
            g_a010_clock.world_ready_seen = true;
            if (probe_enabled()) {
                std::fprintf(stderr,
                    "[a010-clock] first world mesh ready; "
                    "starting authored playback at frame %.0f\n",
                    (double)g_a010_clock.playback_frame);
                std::fflush(stderr);
            }
        }
        if (g_a010_clock.world_ready_seen &&
            !g_a010_clock.animation_ready_seen &&
            g_yz_a010_animation_ready != 0) {
            g_a010_clock.anchor = now;
            /*
             * Keep the exact authored time that produced this palette.
             * Jumping or rewinding only root+0x210 does not replay the
             * character-track callbacks, so it pairs bones from one moment
             * with a camera from another. Hold this time briefly, then resume
             * forward-only playback one authored frame per scene update.
             */
            g_a010_clock.animation_ready_seen = true;
            g_a010_clock.animation_settle_updates = 16;
            g_yz_a010_reference_camera_active = 1;
            if (FILE* acceptance =
                    std::fopen("scratch\\a010_acceptance.txt", "w")) {
                std::fprintf(acceptance,
                    "natural-sync-armed timeline=%.0f animation-ready=1\n",
                    (double)g_a010_clock.playback_frame);
                std::fclose(acceptance);
            }
            /*
             * The surface/geometry verifier is intentionally armed here, not
             * at scene open: synchronous readbacks during the hidden model
             * warm-up made the acceptance run needlessly slow.
             */
            rsx_live_draw_a010_probe_begin();
            if (probe_enabled()) {
                std::fprintf(stderr,
                    "[a010-clock] authored animation palette ready; "
                    "synchronizing verified camera at frame %.0f\n",
                    (double)g_a010_clock.playback_frame);
                std::fflush(stderr);
            }
        }
        if (g_a010_clock.camera_ready_seen) {
            /*
             * The debug recompiler currently presents at roughly 7 FPS.
             * Advancing the authored 30-FPS timeline from wall time skips most
             * of this short shot (and looks "super fast"). Tie it to the
             * scene's actual update cadence so every authored frame renders.
             */
            corrected_value = g_a010_clock.playback_frame;
            if (!g_a010_clock.reference_probe_seen &&
                corrected_value >= 560.0f) {
                /*
                 * The healthy Sunshine Orphanage reference was captured at
                 * CMT/AUTH frame 612.  Normal playback arms shortly before it;
                 * the explicit reference probe starts directly on that frame.
                 */
                rsx_live_draw_a010_probe_begin();
                g_a010_clock.reference_probe_seen = true;
            }
            if (g_a010_clock.world_ready_seen &&
                a010_camera_available(corrected_value) &&
                !std::getenv("YZ_A010_START_REFERENCE")) {
                if (!g_a010_clock.animation_ready_seen) {
                    /*
                     * Bounded hidden warm-up: this is the minimum forward
                     * evaluation needed for the asynchronous controller to
                     * publish.  Never cross the opening track's 1068 end.
                     */
                    if (g_a010_clock.playback_frame < 1000.0f) {
                        const float step =
                            std::getenv("YZ_A010_FAST_WARMUP") ? 10.0f : 1.0f;
                        g_a010_clock.playback_frame += step;
                        if (g_a010_clock.playback_frame > 1000.0f)
                            g_a010_clock.playback_frame = 1000.0f;
                    }
                } else if (g_a010_clock.animation_settle_updates != 0) {
                    g_a010_clock.animation_settle_updates--;
                } else {
                    g_a010_clock.playback_frame +=
                        std::getenv("YZ_A010_FAST_CLOCK") ? 10.0f : 1.0f;
                }
            }
        } else {
            corrected_value = 0.0f;
        }
        write_float(root + 0x210u, corrected_value);
        /*
         * Keep the renderer-side fallback current even if the native camera
         * consumer is skipped after a clip transition.  This is once per AUTH
         * update; func_00E7E1EC still installs the same matrix at its ordinary
         * consumer boundary when that path runs.
         */
        if (g_a010_clock.camera_ready_seen &&
            a010_camera_available(corrected_value))
            apply_a010_cmt_camera(corrected_value);
    } else if (g_a010_clock.active) {
        g_a010_clock.active = false;
        g_yz_a010_reference_camera_active = 0;
        rsx_live_draw_set_a010_camera_matrix(nullptr);
        g_a010_camera_ready.store(false, std::memory_order_release);
        for (cmt_clip& clip : g_a010_cmt) {
            clip.address = 0;
            clip.frames = 0;
            clip.snapshot_valid = false;
        }
    }

    if (enabled) {
        const unsigned long n = ++g_clock_calls;
        if (sample(n)) {
            std::fprintf(stderr,
                "[a010-clock] clock-update n=%lu tid=%u root=%08X "
                "before=%.3f guest=%.3f after=%.3f frame=%u flags=%08X%s\n",
                n, yz_thread_current_id(), root, (double)before,
                (double)(repair_clock ? guest_value
                                     : read_float(root + 0x210u)),
                (double)read_float(root + 0x210u),
                vm_read32(root + 0x208u), vm_read32(root + 0x204u),
                repair_clock ? " host-30hz" : "");
            std::fflush(stderr);
        }
    }
}

void func_00E7E1EC(ppu_context* ctx)
{
    /*
     * This is the last consumer of the 0x01622650 view-projection matrix.
     * AUTH updates earlier in the frame, but the native camera reset can
     * replace that matrix with NaNs afterwards. Install the reconstructed
     * CMT camera here so the lifted routine consumes it immediately.
     */
    const bool repair_camera =
        g_a010_clock.active &&
        g_a010_clock.camera_ready_seen &&
        is_a010_root(g_a010_clock.root);
    if (repair_camera) {
        apply_a010_cmt_camera(
            a010_base_frame(g_a010_clock.root) +
            read_float(g_a010_clock.root + 0x210u));
    }
    func_00E7E1EC_lifted(ctx);
    if (repair_camera) {
        /*
         * The native path composes the view-projection with a companion
         * affine matrix that is also NaN in a010, then uploads the result to
         * vertex constants c108..c111.  The CMT reconstruction is the final
         * view-projection: at a010 frame 612 it reproduces the healthy RPCS3
         * capture's draw-520 constants exactly. Upload those four rows
         * directly after the invalid native upload.
         */
        ctx->gpr[4] = 0x6Cu;
        ctx->gpr[5] = 0x01622650u;
        func_00EB9D84(ctx);
        drain_trampoline(ctx);
    }
}

void func_00C98028(ppu_context* ctx)
{
    const uint32_t root = (uint32_t)ctx->gpr[3];
    const bool enabled = probe_enabled();
    const float before = enabled ? read_float(root + 0x210u) : 0.0f;
    const unsigned long n = enabled ? ++g_state16_calls : 0;

    func_00C98028_lifted(ctx);

    if (enabled && sample(n)) {
        std::fprintf(stderr,
            "[a010-clock] state16 n=%lu tid=%u root=%08X "
            "before=%.3f after=%.3f frame=%u active=%08X flags=%08X/%08X "
            "aggregate_calls=%lu clock_calls=%lu\n",
            n, yz_thread_current_id(), root, (double)before,
            (double)read_float(root + 0x210u),
            vm_read32(root + 0x208u), vm_read32(root + 0x218u),
            vm_read32(root + 0x204u), vm_read32(root + 0x280u),
            g_aggregate_calls.load(), g_clock_calls.load());
        std::fflush(stderr);
    }
}
