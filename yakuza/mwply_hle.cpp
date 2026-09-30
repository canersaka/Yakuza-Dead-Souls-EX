/* mwPly movie HLE.
 *
 * The lifter (--override-funcs yakuza/movie_overrides.txt) emits the listed
 * guest functions as func_XXXXXXXX_lifted while the plain symbols remain
 * available for local implementations. Direct calls, table dispatch, and
 * tail-branches therefore land here.
 *
 * mwPly lifecycle calls own a host FFmpeg session instead of the PS3
 * Sofdec/SPURS decode pipeline. The direct host presenter plays the movie at
 * its authored rate. If it cannot start (no live renderer or no FFmpeg), the
 * session falls back to decoding here and writing frames into the movie
 * surface the game already samples. GetStat/GetTime return the
 * title-observed status and clock. PLAYEND comes only from real host media
 * EOS, and teardown remains game-initiated.
 */
#include "ppu_recomp.h"
#include "../libs/codec/movie_ffmpeg.h"
#include "../libs/audio/cellAudio.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

extern "C" uint8_t  vm_read8(uint64_t addr);
extern "C" uint32_t vm_read32(uint64_t addr);
extern "C" void     vm_write32(uint64_t addr, uint32_t val);
extern "C" long     yz_host_movie_start(const char* host_path);
extern "C" void     yz_host_movie_stop(long serial);
extern "C" int      yz_host_movie_status(long serial);
extern "C" void     yz_host_movie_time(long serial, uint32_t* count,
                                        uint32_t* scale);
extern "C" void     yz_host_movie_graphics_session_closed(long serial);

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
    unsigned frame_id = 0;
    bool eof = false;
    bool stopping = false;
    bool host_audio = false;
    char host_path[320] = {};
    LARGE_INTEGER t0{}, freq{};
    unsigned long long tex_targets[2] = {0, 0};  /* observed ping-pong EAs */
};

MwhleSession g_sess;   /* the game runs one movie player instance */

/* The title reuses its one mwPly handle as the clock source for subsequent
 * in-engine AUTH scenes. A native Start normally resets that clock, but the
 * direct host route intentionally skips native Start to avoid launching the
 * broken PS3 decode pipeline. Keep a stopped-handle clock which begins on its
 * first post-movie GetTime call, rather than at movie teardown: menu/loading
 * time must not be charged to the next scene. */
struct MwhleRetiredClock {
    unsigned long long handle = 0;
    bool running = false;
    LARGE_INTEGER t0{}, freq{};
};

MwhleRetiredClock g_retired_clock;

double now_sec(MwhleSession& s)
{
    if (s.direct_serial > 0) {
        uint32_t count = 0, scale = 1;
        yz_host_movie_time(s.direct_serial, &count, &scale);
        return scale ? (double)count / scale : 0.0;
    }
    if (s.host_audio && !cellAudioHostStreamFinished())
        return (double)cellAudioHostStreamPositionFrames() / 48000.0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const double qpc =
        (double)(t.QuadPart - s.t0.QuadPart) / (double)s.freq.QuadPart;
    return qpc;
}

/* Guest '/dev_bdvd/...' paths map to 'gamedata/dev_bdvd/...' on the host. */
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
    g_retired_clock = MwhleRetiredClock{};
    char guest[256] = {};
    for (size_t i = 0; i + 1 < sizeof(guest); i++) {
        guest[i] = (char)vm_read8(fname_ea + i);
        if (!guest[i]) break;
    }
    char host[320];
    map_host_path(guest, host, sizeof(host));
    MwhleSession& s = g_sess;
    s.handle = handle;
    snprintf(s.host_path, sizeof(s.host_path), "%s", host);
    s.direct_serial = yz_host_movie_start(host);
    if (s.direct_serial > 0) {
        QueryPerformanceFrequency(&s.freq);
        QueryPerformanceCounter(&s.t0);
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
    if (movie_has_audio(s.mv))
        s.host_audio = cellAudioHostStreamStart(
            movie_audio_s16(s.mv), movie_audio_frames(s.mv), 1) == 0;
}

bool session_active(const MwhleSession& s)
{
    return s.direct_serial > 0 || s.mv != nullptr;
}

void ensure_decoded(MwhleSession& s)
{
    if (s.cur_valid || s.eof || !s.mv || s.stopping) return;
    /* The movie plane samples byte 0 as R, so frames are decoded as RGBA. */
    s.cur_argb = movie_next_rgba(s.mv, &s.cur_pts);
    if (s.cur_argb) {
        s.cur_valid = true;
    } else {
        s.eof = true;
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

/* The guest compositor can currently flip below the movie's authored rate.
 * Decode past frames whose presentation time has already elapsed instead of
 * stretching the movie and its subtitle timeline. The latest due frame is
 * still delivered through the ordinary game surface; only obsolete pictures
 * are dropped, exactly as a real-time player does under load. */
void catch_up_to_clock(MwhleSession& s)
{
    ensure_decoded(s);
    const double clock = now_sec(s);
    const double frame_span = 1.0 / s.fps;
    unsigned dropped = 0;
    while (s.cur_valid && !s.eof) {
        const double pts = s.cur_pts > 0.0
            ? s.cur_pts : (double)s.frame_id / s.fps;
        if (pts + frame_span >= clock)
            break;
        s.cur_valid = false;
        s.frame_id++;
        dropped++;
        ensure_decoded(s);
        if (dropped >= 240)
            break;
    }
}

extern "C" uint8_t* vm_base;

/* Pixel delivery into the movie plane. The plane is a 2-buffer ping-pong of
 * linear A8R8G8B8 1280x720 (pitch = w*4) in RSX LOCAL memory (0xC0000000
 * band), taken from the game's RSX command stream. Buffer EAs come from the game's call
 * state: r8 at GetCurFrm carries the current target. Both ping-pong EAs are
 * recorded as they appear, and every recorded target receives each frame. */
void write_pixels(MwhleSession& s, ppu_context* ctx)
{
    const unsigned long long r8 = ctx->gpr[8];
    if (r8 >= 0xC0000000ull && r8 < 0xD0000000ull) {
        if (s.tex_targets[0] != r8 && s.tex_targets[1] != r8) {
            const int slot = s.tex_targets[0] ? 1 : 0;
            s.tex_targets[slot] = r8;
        }
    }
    const size_t bytes = (size_t)s.w * (size_t)s.h * 4;
    for (int i = 0; i < 2; i++) {
        if (!s.tex_targets[i]) continue;
        memcpy(vm_base + (uint32_t)s.tex_targets[i], s.cur_argb, bytes);
    }
}

/* Fill the decoded-frame descriptor consumed by the title. The caption path
 * reads the per-file frame and time slots in addition to the display fields,
 * so every observed slot is initialized.
 * Offset 0x00 is also used as a signed valid-frame gate before conversion
 * into the title's output plane. Decoded pixels are already present there;
 * zero is therefore valid, while -1 continues to mean "no frame". */
void serve_frame(MwhleSession& s, unsigned long long out_ea, ppu_context* ctx)
{
    for (unsigned off = 0; off < 0xA8; off += 4)
        vm_write32(out_ea + off, 0);
    const uint32_t frame_time_ms =
        (uint32_t)((s.cur_pts > 0.0 ? s.cur_pts
                                   : (double)s.frame_id / s.fps) * 1000.0 + 0.5);
    vm_write32(out_ea + 0x00, 0);              /* valid-frame gate */
    vm_write32(out_ea + 0x04, s.frame_id);     /* frame identifier */
    vm_write32(out_ea + 0x08, 1);              /* pixel format */
    vm_write32(out_ea + 0x0C, (uint32_t)s.w);  /* width */
    vm_write32(out_ea + 0x10, (uint32_t)s.h);  /* height */
    vm_write32(out_ea + 0x14, (uint32_t)s.w);  /* display width */
    vm_write32(out_ea + 0x18, (uint32_t)s.h);  /* display height */
    vm_write32(out_ea + 0x1C, ((uint32_t)s.w + 15u) / 16u); /* horizontal macroblock count */
    vm_write32(out_ea + 0x20, ((uint32_t)s.h + 15u) / 16u); /* vertical macroblock count */
    vm_write32(out_ea + 0x24, 1);              /* intra-frame picture type */
    vm_write32(out_ea + 0x28, s.fps_x1000);    /* fps * 1000 */
    vm_write32(out_ea + 0x2C, s.frame_id);     /* stream frame index */
    vm_write32(out_ea + 0x30, frame_time_ms);  /* stream time */
    vm_write32(out_ea + 0x34, 1000);           /* time units per second */
    vm_write32(out_ea + 0x38, 0);              /* concatenation count */
    vm_write32(out_ea + 0x3C, s.frame_id);     /* file frame index */
    vm_write32(out_ea + 0x40, frame_time_ms);  /* file time */
    vm_write32(out_ea + 0x44, 0);              /* decode error count */
    vm_write32(out_ea + 0x48, 0);              /* received frame count */
    vm_write32(out_ea + 0x4C, 0);              /* user-data pointer */
    vm_write32(out_ea + 0x50, 0);              /* user-data size */
    vm_write32(out_ea + 0x54, 1);              /* progressive frame */
    vm_write32(out_ea + 0x58, 1);              /* BT.601 color matrix */
    vm_write32(out_ea + 0x5C, 0);              /* zmin */
    vm_write32(out_ea + 0x60, 0xFFFFFFFFu);    /* zmax */
    vm_write32(out_ea + 0x64, 1);              /* MPEG-1 video */
    /* Codec details (+0x68..+0x9C) and auxiliary data (+0xA0..+0xA4)
     * are intentionally zero: this title obtains localized captions from
     * scenario/caption.bin rather than the SFD subtitle channel. */
    write_pixels(s, ctx);
    s.cur_valid = false;
    s.frame_id++;
}

} // namespace

/* ------------------------------------------------------------ overrides -- */

/* Start opens the host session and, when it is active, skips the native
 * Start so the PS3 Sofdec/SPURS pipeline never runs in parallel. The native
 * Start remains the fallback when no session could be opened. */
static void mwhle_start_common(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    const unsigned long long handle = ctx->gpr[3];
    const unsigned long long fname  = ctx->gpr[4];
    session_open(handle, fname);
    if (session_active(g_sess))
        return;
    lifted(ctx);
}
void func_00F4E720(ppu_context* ctx) { mwhle_start_common(ctx, func_00F4E720_lifted); }
void func_00F51658(ppu_context* ctx) { mwhle_start_common(ctx, func_00F51658_lifted); }

/* IsNextFrmReady is called only by the native GetCurFrm worker's ring walk.
 * With a host session there is no native ring entry to advance to, so report
 * "not ready"; the game acquires host frames through GetCurFrm/GetFrm. */
void func_00F4D0A8(ppu_context* ctx)
{
    if (session_active(g_sess)) {
        ctx->gpr[3] = 0;
        return;
    }
    func_00F4D0A8_lifted(ctx);
}

void func_00F4D878(ppu_context* ctx)   /* GetCurFrm(handle, out_frame*) */
{
    const unsigned long long out_ea = ctx->gpr[4];
    /* The caller partially trusts fields of its stack descriptor beyond the
     * no-frame marker at +0x0, so clear the whole descriptor first. */
    for (unsigned off = 0; off < 0xA8; off += 4)
        vm_write32(out_ea + off, 0);
    if (g_sess.direct_serial > 0) {
        vm_write32(out_ea, 0xFFFFFFFFu);
        ctx->gpr[3] = g_sess.handle;
        return;
    }
    if (g_sess.mv) {
        MwhleSession& s = g_sess;
        catch_up_to_clock(s);
        if (s.cur_valid && frame_due(s))
            serve_frame(s, out_ea, ctx);
        else
            vm_write32(out_ea, 0xFFFFFFFFu);
        ctx->gpr[3] = s.handle;
        return;
    }
    func_00F4D878_lifted(ctx);
}

void func_00F4DA90(ppu_context* ctx)   /* GetFrm(handle, int* out_status, out_frame*) */
{
    const unsigned long long stat_ea = ctx->gpr[4];
    const unsigned long long out_ea  = ctx->gpr[5];
    for (unsigned off = 0; off < 0xA8; off += 4)
        vm_write32(out_ea + off, 0);
    if (g_sess.direct_serial > 0) {
        vm_write32(stat_ea, 0xFFFFFFFFu);
        vm_write32(out_ea, 0xFFFFFFFFu);
        return;
    }
    if (g_sess.mv) {
        MwhleSession& s = g_sess;
        catch_up_to_clock(s);
        if (s.cur_valid && frame_due(s)) {
            serve_frame(s, out_ea, ctx);
            vm_write32(stat_ea, 0);
        } else {
            vm_write32(stat_ea, 0xFFFFFFFFu);
            vm_write32(out_ea, 0xFFFFFFFFu);
        }
        return;
    }
    func_00F4DA90_lifted(ctx);
}

static void mwhle_release_common(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    if (!session_active(g_sess))
        lifted(ctx);
}
void func_00F4D134(ppu_context* ctx) { mwhle_release_common(ctx, func_00F4D134_lifted); }
void func_00F4D210(ppu_context* ctx) { mwhle_release_common(ctx, func_00F4D210_lifted); }

/* Teardown remains game-initiated. The native object still exists even when
 * direct playback skipped its Start, so native Stop/Destroy retain ordinary
 * object cleanup while host cancellation only stops media delivery. */
void func_00F4ED44(ppu_context* ctx)   /* Stop */
{
    if (session_active(g_sess) && ctx->gpr[3] == g_sess.handle) {
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
    if (session_active(g_sess) && ctx->gpr[3] == g_sess.handle) {
        g_sess.stopping = true;
        if (g_sess.direct_serial > 0)
            yz_host_movie_stop(g_sess.direct_serial);
        else if (g_sess.host_audio) {
            cellAudioHostStreamStop();
            g_sess.host_audio = false;
        }
    }
    func_00F4E608_lifted(ctx);
}
void func_00F4A9AC(ppu_context* ctx)   /* Destroy */
{
    const unsigned long long handle = ctx->gpr[3];
    func_00F4A9AC_lifted(ctx);
    if (session_active(g_sess) && handle == g_sess.handle) {
        const long direct_serial = g_sess.direct_serial;
        session_close(g_sess);
        yz_host_movie_graphics_session_closed(direct_serial);
        g_retired_clock.handle = handle;
        QueryPerformanceFrequency(&g_retired_clock.freq);
    }
}

/* GetStat/GetTime are the game-visible CRI contract for direct playback.
 * Status 3 is published only after the host decoder consumes real media EOS. */
void func_00F4FD3C(ppu_context* ctx)
{
    const unsigned long long handle = ctx->gpr[3];
    if (g_sess.direct_serial > 0 && handle == g_sess.handle) {
        ctx->gpr[3] = (uint64_t)yz_host_movie_status(g_sess.direct_serial);
    } else if (g_sess.mv && handle == g_sess.handle) {
        ctx->gpr[3] = g_sess.eof || g_sess.stopping ? 3u : 2u;
    } else {
        func_00F4FD3C_lifted(ctx);
    }
}

void func_00F4F8D0(ppu_context* ctx)   /* GetTime(handle, *ncount, *tscale) */
{
    if (session_active(g_sess) && ctx->gpr[3] == g_sess.handle) {
        uint32_t count = 0, scale = 30;
        if (g_sess.direct_serial > 0) {
            /*
             * The direct presenter starts exactly once at StartFname and owns
             * the playback clock the guest observes here.  Restarting it on
             * GetTime would replay the start of scene movies.
             */
            yz_host_movie_time(g_sess.direct_serial, &count, &scale);
        } else {
            scale = (uint32_t)(g_sess.fps + 0.5);
            if (!scale) scale = 30;
            count = (uint32_t)(now_sec(g_sess) * scale);
        }
        if (ctx->gpr[4]) vm_write32(ctx->gpr[4], count);
        if (ctx->gpr[5]) vm_write32(ctx->gpr[5], scale);
        return;
    }
    if (!session_active(g_sess) && g_retired_clock.handle &&
        ctx->gpr[3] == g_retired_clock.handle) {
        if (!g_retired_clock.running) {
            QueryPerformanceCounter(&g_retired_clock.t0);
            g_retired_clock.running = true;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double seconds =
            (double)(now.QuadPart - g_retired_clock.t0.QuadPart) /
            (double)g_retired_clock.freq.QuadPart;
        const uint32_t scale = 30;
        const uint32_t count =
            seconds > 0.0 ? (uint32_t)(seconds * (double)scale) : 0;
        if (ctx->gpr[4]) vm_write32(ctx->gpr[4], count);
        if (ctx->gpr[5]) vm_write32(ctx->gpr[5], scale);
        return;
    }
    func_00F4F8D0_lifted(ctx);
}
