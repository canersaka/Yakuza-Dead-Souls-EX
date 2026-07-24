/* mwPly video-HLE seam (Route 1 vertical slice, 2026-07-23).
 *
 * The lifter (--override-funcs yakuza/movie_overrides.txt) emits the listed
 * guest functions as func_XXXXXXXX_lifted while the plain symbols remain
 * available for local implementations. Direct calls, table dispatch, and
 * tail-branches therefore land here.
 *
 * DISARMED (default): every override forwards to its lifted body; the only
 * added behavior is bounded logging.
 *
 * ARMED (YZ_MOVIE_HLE=1): mwPly lifecycle calls own a direct host FFmpeg
 * session. Video goes through the already-proven D3D12 presenter and movie
 * audio through cellAudio's host stream; the PS3 Sofdec/SPURS decode work is
 * not started in parallel. GetStat/GetTime expose the documented CRI contract.
 * PLAYEND comes only from real host media EOS, and teardown remains
 * game-initiated. The old fd bridge disables itself while this route is armed.
 */
#include "ppu_recomp.h"
#include "../libs/codec/movie_ffmpeg.h"
#include "../libs/audio/cellAudio.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

extern "C" unsigned yz_thread_current_id(void);
extern "C" uint8_t  vm_read8(uint64_t addr);
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write32(uint64_t addr, uint32_t val);
extern "C" long     yz_host_movie_start(const char* host_path);
extern "C" void     yz_host_movie_stop(long serial);
extern "C" int      yz_host_movie_status(long serial);
extern "C" void     yz_host_movie_time(long serial, uint32_t* count,
                                        uint32_t* scale);

/* ------------------------------------------------------------------ flag -- */

extern "C" int yz_movie_hle_armed(void)
{
    static int armed = -1;
    if (armed < 0) {
        armed = getenv("YZ_MOVIE_HLE") ? 1 : 0;
        fprintf(stderr, "[mwhle] ARMED banner: YZ_MOVIE_HLE=%d (%s)\n", armed,
                armed ? "frame-leaf HLE ACTIVE, fd-bridge disabled"
                      : "pass-through only, fd-bridge default");
        fflush(stderr);
    }
    return armed;
}

/* --------------------------------------------------------------- session -- */

namespace {

struct MwhleSession {
    unsigned long long handle = 0;
    MoviePlayer* mv = nullptr;
    long direct_serial = 0;
    int w = 0, h = 0;
    double fps = 30.0;
    unsigned fps_x1000 = 30000;
    const uint8_t* cur_argb = nullptr;  /* decoded frame, awaiting its time */
    double cur_pts = 0.0;
    bool cur_valid = false;
    bool held = false;        /* served via GetCurFrm, not yet released */
    unsigned frame_id = 0;
    bool eof = false;
    bool stopping = false;
    bool host_audio = false;
    LARGE_INTEGER t0{}, freq{};
    unsigned long long tex_targets[2] = {0, 0};  /* observed ping-pong EAs */
    unsigned writes = 0, no_target_logged = 0;
};

MwhleSession g_sess;   /* the game runs one movie player instance (measured) */

double now_sec(MwhleSession& s)
{
    if (s.direct_serial > 0) {
        uint32_t count = 0, scale = 1;
        yz_host_movie_time(s.direct_serial, &count, &scale);
        return scale ? (double)count / scale : 0.0;
    }
    if (s.host_audio)
        return (double)cellAudioHostStreamPositionFrames() / 48000.0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - s.t0.QuadPart) / (double)s.freq.QuadPart;
}

/* Guest '/dev_bdvd/...' -> host 'gamedata/dev_bdvd/...' (mapping evident from
 * the cellFs layer's own Open logs). */
void map_host_path(const char* guest, char* out, size_t cap)
{
    if (guest[0] == '/')
        snprintf(out, cap, "gamedata%s", guest);
    else
        snprintf(out, cap, "%s", guest);
}

void session_close(MwhleSession& s)
{
    if (s.direct_serial > 0) {
        yz_host_movie_stop(s.direct_serial);
        s.direct_serial = 0;
    }
    if (s.host_audio) {
        cellAudioHostStreamStop();
        s.host_audio = false;
    }
    if (s.mv) { movie_close(s.mv); s.mv = nullptr; }
    s = MwhleSession{};
}

void session_open(unsigned long long handle, unsigned long long fname_ea)
{
    session_close(g_sess);
    char guest[256] = {};
    for (size_t i = 0; i + 1 < sizeof(guest); i++) {
        guest[i] = (char)vm_read8(fname_ea + i);
        if (!guest[i]) break;
    }
    char host[320];
    map_host_path(guest, host, sizeof(host));
    MwhleSession& s = g_sess;
    s.handle = handle;
    s.direct_serial = yz_host_movie_start(host);
    if (s.direct_serial > 0) {
        QueryPerformanceFrequency(&s.freq);
        QueryPerformanceCounter(&s.t0);
        fprintf(stderr,
                "[mwhle] direct host session OPEN handle=%08llX serial=%ld '%s'\n",
                handle, s.direct_serial, host);
        fflush(stderr);
        return;
    }
    s.mv = movie_open(host);
    if (!s.mv) {
        fprintf(stderr, "[mwhle] session OPEN FAILED handle=%08llX host='%s' "
                        "(serving nothing; native pipeline unaffected)\n", handle, host);
        fflush(stderr);
        return;
    }
    s.w = movie_width(s.mv);
    s.h = movie_height(s.mv);
    s.fps = movie_framerate(s.mv);
    if (s.fps < 1.0) s.fps = 30.0;
    s.fps_x1000 = (unsigned)(s.fps * 1000.0 + 0.5);
    QueryPerformanceFrequency(&s.freq);
    QueryPerformanceCounter(&s.t0);
    if (movie_has_audio(s.mv)) {
        const int mix_guest = getenv("YZ_MOVIE_HLE_MIX_GUEST") ? 1 : 0;
        s.host_audio = cellAudioHostStreamStart(
            movie_audio_s16(s.mv), movie_audio_frames(s.mv),
            mix_guest ? 0 : 1) == 0;
    }
    fprintf(stderr, "[mwhle] session OPEN handle=%08llX '%s' %dx%d @ %.3f fps "
                    "audio=%s frames=%llu clock=%s\n",
            handle, host, s.w, s.h, s.fps,
            movie_has_audio(s.mv) ? "FFmpeg ADX" : "native fallback",
            (unsigned long long)movie_audio_frames(s.mv),
            s.host_audio ? "audio" : "QPC");
    fflush(stderr);
}

bool session_active(const MwhleSession& s)
{
    return s.direct_serial > 0 || s.mv != nullptr;
}

void ensure_decoded(MwhleSession& s)
{
    if (s.cur_valid || s.eof || !s.mv || s.stopping) return;
    /* Byte order settled empirically (boot 5): the movie plane samples byte0
     * as R (RGBA order) — black rendered red with ARGB bytes. */
    s.cur_argb = movie_next_rgba(s.mv, &s.cur_pts);
    if (s.cur_argb) {
        s.cur_valid = true;
    } else {
        s.eof = true;
        fprintf(stderr, "[mwhle] host decode EOF handle=%08llX after %u frames "
                        "(no EOS synthesized; native pipeline owns completion)\n",
                s.handle, s.frame_id);
        fflush(stderr);
    }
}

bool frame_due(MwhleSession& s)
{
    if (!s.cur_valid) return false;
    const double t = now_sec(s);
    const double due = (s.cur_pts > 0.0) ? s.cur_pts
                                         : (double)s.frame_id / s.fps;
    return t >= due;
}

extern "C" uint8_t* vm_base;

/* Pixel delivery into the movie plane. The plane is a 2-buffer ping-pong of
 * linear A8R8G8B8 1280x720 (pitch = w*4) in RSX LOCAL memory (0xC0000000
 * band), measured from the RSX oracle. Buffer EAs come from the game's call
 * state: r8 at GetCurFrm carries the current target. Both ping-pong EAs are
 * recorded as they appear, and every recorded target receives each frame. */
void write_pixels(MwhleSession& s, ppu_context* ctx)
{
    const unsigned long long r8 = ctx->gpr[8];
    if (r8 >= 0xC0000000ull && r8 < 0xD0000000ull) {
        if (s.tex_targets[0] != r8 && s.tex_targets[1] != r8) {
            const int slot = s.tex_targets[0] ? 1 : 0;
            s.tex_targets[slot] = r8;
            fprintf(stderr, "[mwhle] learned movie-plane target[%d]=%08llX\n",
                    slot, r8);
            fflush(stderr);
        }
    } else if (s.no_target_logged < 8) {
        s.no_target_logged++;
        fprintf(stderr, "[mwhle] serve regs (no local-range r8): r5=%08llX "
                        "r6=%08llX r7=%08llX r8=%08llX r9=%08llX r10=%08llX\n",
                (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->gpr[6],
                (unsigned long long)ctx->gpr[7], r8,
                (unsigned long long)ctx->gpr[9], (unsigned long long)ctx->gpr[10]);
        fflush(stderr);
    }
    const size_t bytes = (size_t)s.w * (size_t)s.h * 4;
    for (int i = 0; i < 2; i++) {
        if (!s.tex_targets[i]) continue;
        memcpy(vm_base + (uint32_t)s.tex_targets[i], s.cur_argb, bytes);
        s.writes++;
    }
    if (s.writes && (s.writes <= 4 || (s.writes & 511u) == 0)) {
        fprintf(stderr, "[mwhle] pixels written n=%u frame=%u targets=%08llX/%08llX\n",
                s.writes, s.frame_id, s.tex_targets[0], s.tex_targets[1]);
        fflush(stderr);
    }
}

/* Fill the title-observed frame descriptor. Required fields are status at
 * +0x00, dimensions at +0x0C/+0x10, matching identifiers at +0x04/+0x2C,
 * pixel format at +0x08, and scaled frame rate at +0x28. */
void serve_frame(MwhleSession& s, unsigned long long out_ea, ppu_context* ctx)
{
    for (unsigned off = 0; off < 0xA8; off += 4)
        vm_write32(out_ea + off, 0);
    static unsigned fmt = 0;
    if (!fmt) {
        const char* f = getenv("YZ_MOVIE_HLE_FMT");
        fmt = f ? (unsigned)atoi(f) : 1u;      /* enum in {1,2,3}; default 1 pending chase */
        if (fmt < 1 || fmt > 3) fmt = 1;
    }
    vm_write32(out_ea + 0x00, 0);              /* status: non-negative = frame */
    vm_write32(out_ea + 0x04, s.frame_id);     /* MUST equal +0x2C */
    vm_write32(out_ea + 0x08, fmt);
    vm_write32(out_ea + 0x0C, (uint32_t)s.w);
    vm_write32(out_ea + 0x10, (uint32_t)s.h);
    vm_write32(out_ea + 0x14, (uint32_t)s.w);
    vm_write32(out_ea + 0x18, (uint32_t)s.h);
    vm_write32(out_ea + 0x28, s.fps_x1000);
    vm_write32(out_ea + 0x2C, s.frame_id);
    vm_write32(out_ea + 0x38, s.frame_id);
    write_pixels(s, ctx);
    s.cur_valid = false;
    s.held = true;
    s.frame_id++;
}

/* ----------------------------------------------------------- logging ------ */

struct call_counter { unsigned long n = 0; bool log_now() { ++n; return n <= 32 || (n & 255u) == 0; } };

void log_call(const char* name, call_counter& c, ppu_context* ctx)
{
    if (!c.log_now()) return;
    fprintf(stderr, "[mwhle] %s n=%lu tid=%u r3=%08llX r4=%08llX r5=%08llX lr=%08llX\n",
            name, c.n, yz_thread_current_id(),
            (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->gpr[4],
            (unsigned long long)ctx->gpr[5], (unsigned long long)ctx->lr);
    fflush(stderr);
}

} // namespace

/* ------------------------------------------------------------ overrides -- */

#define MWHLE_PASSTHROUGH(addr, name)                                  \
    void func_##addr(ppu_context* ctx)                                 \
    {                                                                  \
        static call_counter c;                                         \
        yz_movie_hle_armed();                                          \
        log_call(name, c, ctx);                                        \
        func_##addr##_lifted(ctx);                                     \
    }

/* Start: native start ALWAYS runs (audio, streaming, status machine all
 * native). Armed adds the host decode session, keyed by handle. */
static void mwhle_start_common(const char* tag, ppu_context* ctx,
                               void (*lifted)(ppu_context*))
{
    static call_counter c;
    const unsigned long long handle = ctx->gpr[3];
    const unsigned long long fname  = ctx->gpr[4];
    log_call(tag, c, ctx);
    if (yz_movie_hle_armed()) {
        session_open(handle, fname);
        /* Direct host playback owns decode, cadence, audio and status. Do not
         * start the PS3 Sofdec/SPURS pipeline in parallel. */
        if (g_sess.direct_serial > 0)
            return;
    }
    lifted(ctx);
}
void func_00F4E720(ppu_context* ctx) { mwhle_start_common("StartFname",   ctx, func_00F4E720_lifted); }
void func_00F51658(ppu_context* ctx) { mwhle_start_common("StartFnameLp", ctx, func_00F51658_lifted); }

/* HYBRID leaves (first-light lesson, mwhle_armed1: replacing the native leaf
 * outright also removed its internal queue upkeep, constipating the whole CRI
 * pipeline — reads stopped, audio starved, PLAYEND never came. So: ALWAYS run
 * the native leaf first (side effects identical to the measured-working native
 * boot; it returns no-frame since the SPU decoder produces nothing), then
 * overlay the host frame on the out-struct only when one is due. Armed-mode
 * divergence from the native configuration = the struct contents, nothing
 * else. */
/* IsNextFrmReady: PURE pass-through in every mode. Its only caller is the
 * native GetCurFrm worker's internal ring walk (census-measured) — boots 2/3
 * crashed because overlaying a host "1" here made that worker advance past a
 * native ring entry that doesn't exist. The game acquires frames via
 * GetCurFrm/GetFrm; the overlay lives there and only there. */
void func_00F4D0A8(ppu_context* ctx)
{
    static call_counter c;
    log_call("IsNextFrmReady", c, ctx);
    if (yz_movie_hle_armed() && g_sess.direct_serial > 0) {
        ctx->gpr[3] = 0; /* direct presenter has no guest frame backlog */
        return;
    }
    func_00F4D0A8_lifted(ctx);
}

void func_00F4D878(ppu_context* ctx)   /* GetCurFrm(handle, out_frame*) */
{
    static call_counter c;
    log_call("GetCurFrm", c, ctx);
    const unsigned long long out_ea = ctx->gpr[4];
    const int armed = yz_movie_hle_armed();
    /* Pre-zero the caller's stack struct: the native empty tail writes ONLY
     * +0x0, leaving stale bytes the caller partially trusts (boot-2 crash:
     * a stale +0x2C fooled the yield check and the game ate garbage). Zeros
     * reproduce the benign state the real game's masked corner relies on. */
    if (armed)
        for (unsigned off = 0; off < 0xA8; off += 4)
            vm_write32(out_ea + off, 0);
    if (armed && g_sess.direct_serial > 0) {
        vm_write32(out_ea, 0xFFFFFFFFu);
        ctx->gpr[3] = g_sess.handle;
        return;
    }
    func_00F4D878_lifted(ctx);
    if (!armed) return;
    if (vm_read32(out_ea + 0x0C) != 0) {   /* only a REAL native fill writes W */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[mwhle] WARNING: native GetCurFrm produced a real "
                            "frame; host overlay yielding\n");
            fflush(stderr);
        }
        return;
    }
    MwhleSession& s = g_sess;
    ensure_decoded(s);
    if (s.mv && s.cur_valid && frame_due(s))
        serve_frame(s, out_ea, ctx);   /* overlay; else zeroed no-frame stands */
}

void func_00F4DA90(ppu_context* ctx)   /* GetFrm(handle, int* out_status, out_frame*) */
{
    static call_counter c;
    log_call("GetFrm", c, ctx);
    const unsigned long long stat_ea = ctx->gpr[4];
    const unsigned long long out_ea  = ctx->gpr[5];
    const int armed = yz_movie_hle_armed();
    if (armed)
        for (unsigned off = 0; off < 0xA8; off += 4)
            vm_write32(out_ea + off, 0);
    if (armed && g_sess.direct_serial > 0) {
        vm_write32(stat_ea, 0xFFFFFFFFu);
        vm_write32(out_ea, 0xFFFFFFFFu);
        return;
    }
    func_00F4DA90_lifted(ctx);
    if (!armed) return;
    if (vm_read32(out_ea + 0x0C) != 0) return;   /* real native fill — yield */
    MwhleSession& s = g_sess;
    ensure_decoded(s);
    if (s.mv && s.cur_valid && frame_due(s)) {
        serve_frame(s, out_ea, ctx);
        vm_write32(stat_ea, 0);
    }
}

static void mwhle_release_common(const char* tag, ppu_context* ctx,
                                 void (*lifted)(ppu_context*))
{
    static call_counter c;
    log_call(tag, c, ctx);
    if (!(yz_movie_hle_armed() && g_sess.direct_serial > 0))
        lifted(ctx);
    if (yz_movie_hle_armed())
        g_sess.held = false;
}
void func_00F4D134(ppu_context* ctx) { mwhle_release_common("RelCurFrm", ctx, func_00F4D134_lifted); }
void func_00F4D210(ppu_context* ctx) { mwhle_release_common("RelFrm",    ctx, func_00F4D210_lifted); }

/* Teardown remains game-initiated. The native object still exists even when
 * direct playback skipped its Start, so native Stop/Destroy retain ordinary
 * object cleanup while host cancellation only stops media delivery. */
void func_00F4ED44(ppu_context* ctx)   /* Stop */
{
    static call_counter c;
    log_call("Stop", c, ctx);
    if (yz_movie_hle_armed() && session_active(g_sess) &&
        ctx->gpr[3] == g_sess.handle) {
        g_sess.stopping = true;
        if (g_sess.direct_serial > 0) {
            yz_host_movie_stop(g_sess.direct_serial);
        } else if (g_sess.host_audio) {
            cellAudioHostStreamStop();
            g_sess.host_audio = false;
        }
    }
    func_00F4ED44_lifted(ctx);
}
void func_00F4E608(ppu_context* ctx)   /* RequestStop */
{
    static call_counter c;
    log_call("RequestStop", c, ctx);
    if (yz_movie_hle_armed() && g_sess.direct_serial > 0 &&
        ctx->gpr[3] == g_sess.handle) {
        g_sess.stopping = true;
        yz_host_movie_stop(g_sess.direct_serial);
    }
    func_00F4E608_lifted(ctx);
}
void func_00F4A9AC(ppu_context* ctx)   /* Destroy */
{
    static call_counter c;
    log_call("Destroy", c, ctx);
    const unsigned long long handle = ctx->gpr[3];
    func_00F4A9AC_lifted(ctx);
    if (yz_movie_hle_armed() && session_active(g_sess) &&
        handle == g_sess.handle) {
        fprintf(stderr, "[mwhle] session CLOSE handle=%08llX frames_served=%u\n",
                g_sess.handle, g_sess.frame_id);
        fflush(stderr);
        session_close(g_sess);
    }
}

/* GetStat/GetTime are the game-visible CRI contract for direct playback.
 * Status 3 is published only after the host decoder consumes real media EOS. */
void func_00F4FD3C(ppu_context* ctx)
{
    static call_counter c;
    static unsigned long long last_handle = 0;
    static unsigned long long last_status = ~0ull;
    yz_movie_hle_armed();
    const unsigned long long handle = ctx->gpr[3];
    if (g_sess.direct_serial > 0 && handle == g_sess.handle)
        ctx->gpr[3] = (uint64_t)yz_host_movie_status(g_sess.direct_serial);
    else
        func_00F4FD3C_lifted(ctx);
    const unsigned long long status = ctx->gpr[3];
    c.n++;
    if (c.n <= 8 || handle != last_handle || status != last_status) {
        fprintf(stderr, "[mwhle] GetStatWrap n=%lu tid=%u handle=%08llX status=%llu%s\n",
                c.n, yz_thread_current_id(), handle, status,
                (status != last_status && c.n > 1) ? " <- TRANSITION" : "");
        fflush(stderr);
        last_handle = handle;
        last_status = status;
    }
}

void func_00F4F8D0(ppu_context* ctx)   /* GetTime(handle, *ncount, *tscale) */
{
    static call_counter c;
    log_call("GetTime", c, ctx);
    if (yz_movie_hle_armed() && g_sess.direct_serial > 0 &&
        ctx->gpr[3] == g_sess.handle) {
        uint32_t count = 0, scale = 30;
        yz_host_movie_time(g_sess.direct_serial, &count, &scale);
        if (ctx->gpr[4]) vm_write32(ctx->gpr[4], count);
        if (ctx->gpr[5]) vm_write32(ctx->gpr[5], scale);
        return;
    }
    func_00F4F8D0_lifted(ctx);
}
