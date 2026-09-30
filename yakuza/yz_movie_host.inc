/* Host movie playback and the render window thread.
 * Part of import_overrides.cpp, which includes its parts once, in order. */

/* Host movie presentation. The mwPly HLE (mwply_hle.cpp) queues a movie by
 * serial at the game's Start; the window thread decodes and presents it and
 * publishes completion, which the HLE reports to the game as PLAYEND. */
static SRWLOCK g_movie_open_lock = SRWLOCK_INIT;
static char g_movie_open_path[MAX_PATH];
static volatile LONG g_movie_open_serial = 0;
static volatile LONG g_movie_completed_serial = 0;
static volatile LONG g_movie_cancelled_serial = 0;
static LONG g_movie_played_serial = 0;
static volatile LONG g_movie_presenting_serial = 0;
static volatile LONG g_movie_presented_frames = 0;
static volatile LONG g_movie_present_timescale = 30;
static volatile LONG g_movie_playing_observed_serial = 0;

static void yz_movie_request_cancel(LONG serial, const char* reason);

/* mwPly HLE control surface, keyed by the game's own Start/Stop lifecycle. */
extern "C" long yz_host_movie_start(const char* host_path)
{
    if (!host_path || !*host_path || !rsx_live_draw_enabled() ||
        !movie_ffmpeg_available())
        return 0;
    AcquireSRWLockExclusive(&g_movie_open_lock);
    strncpy(g_movie_open_path, host_path, sizeof(g_movie_open_path) - 1);
    g_movie_open_path[sizeof(g_movie_open_path) - 1] = '\0';
    const LONG serial = InterlockedIncrement(&g_movie_open_serial);
    InterlockedExchange(&g_movie_presented_frames, 0);
    InterlockedExchange(&g_movie_present_timescale, 30);
    ReleaseSRWLockExclusive(&g_movie_open_lock);
    fprintf(stderr, "[movie] mwPly queued direct host playback serial=%ld '%s'\n",
            serial, host_path);
    fflush(stderr);
    return serial;
}

extern "C" void yz_host_movie_stop(long serial)
{
    yz_movie_request_cancel((LONG)serial, "game mwPly Stop/RequestStop");
}

extern "C" int yz_host_movie_status(long serial)
{
    if (serial <= 0) return 0;
    if (InterlockedCompareExchange(&g_movie_completed_serial, 0, 0) >= serial)
        return 3; /* MWSFD_STAT_PLAYEND */
    if (InterlockedCompareExchange(&g_movie_presenting_serial, 0, 0) == serial) {
        InterlockedExchange(&g_movie_playing_observed_serial, serial);
        return 2; /* MWSFD_STAT_PLAYING */
    }
    return 1;     /* MWSFD_STAT_PREP */
}

extern "C" void yz_host_movie_time(long serial, uint32_t* count,
                                    uint32_t* scale)
{
    if (!count || !scale || serial <= 0) return;
    const LONG timescale =
        InterlockedCompareExchange(&g_movie_present_timescale, 0, 0);
    uint64_t audio_frames = cellAudioHostStreamPositionFrames();
    *scale = timescale > 0 ? (uint32_t)timescale : 30u;
    if (audio_frames) {
        *count = (uint32_t)((audio_frames * *scale) / 48000u);
    } else {
        *count = (uint32_t)InterlockedCompareExchange(
            &g_movie_presented_frames, 0, 0);
    }
}

static void yz_movie_request_cancel(LONG serial, const char* reason)
{
    if (serial <= 0)
        return;
    AcquireSRWLockExclusive(&g_movie_open_lock);
    if (serial > g_movie_completed_serial && serial > g_movie_cancelled_serial) {
        InterlockedExchange(&g_movie_cancelled_serial, serial);
        fprintf(stderr, "[movie] cancel requested serial=%ld (%s)\n",
                serial, reason ? reason : "unspecified");
        fflush(stderr);
    }
    ReleaseSRWLockExclusive(&g_movie_open_lock);
}

/* The a010 auth sequence is active from its package open until the a020
 * movie or package opens. dispatch.cpp and presentation_clock_hle.cpp (through
 * g_yz_a010_animation_ready) key off this latch. */
static void yz_auth_open_hook(CellFsFd fd, const char* guest_path,
                              const char* host_path)
{
    (void)fd;
    (void)host_path;
    if (!guest_path)
        return;
    if (strstr(guest_path, "/auth/a010/a010.par")) {
        InterlockedExchange(&g_yz_a010_root_active, 1);
        InterlockedExchange(&g_yz_a010_animation_ready, 0);
    } else if (strstr(guest_path, "/movie/a020.sfd") ||
               strstr(guest_path, "/auth/a020/a020.par")) {
        InterlockedExchange(&g_yz_a010_root_active, 0);
    }
}

static void yz_movie_complete(LONG serial)
{
    AcquireSRWLockExclusive(&g_movie_open_lock);
    if (serial > g_movie_completed_serial)
        InterlockedExchange(&g_movie_completed_serial, serial);
    ReleaseSRWLockExclusive(&g_movie_open_lock);
}

/* The FIFO consumer is the sole guest caller of rsx_live_draw_method(), and
 * it holds g_rsx_fifo_lock across each complete method dispatch.  Take that
 * same lock while transferring command-list ownership to or from the host
 * movie thread.  This drains any in-flight guest Close/reset and prevents a
 * new one from starting during the handoff, without adding synchronization
 * to the per-method render hot path.
 *
 * The movie owns graphics from the moment decode starts until the guest
 * destroys the player, so no live-draw section spans movie preparation.  The
 * session serial is tracked so a late close from an older player never
 * disarms a newer movie. */
static volatile LONG g_movie_live_draw_serial = 0;

static void yz_movie_begin_live_draw_mode(LONG serial)
{
    yz_rsx_fifo_acquire();
    rsx_live_draw_set_movie_mode(1);
    InterlockedExchange(&g_movie_live_draw_serial, serial);
    yz_rsx_fifo_release();
}

static void yz_movie_end_live_draw_mode(LONG serial)
{
    yz_rsx_fifo_acquire();
    if (InterlockedCompareExchange(
            &g_movie_live_draw_serial, 0, serial) == serial) {
        rsx_live_draw_set_movie_mode(0);
    }
    yz_rsx_fifo_release();
}

extern "C" void yz_host_movie_graphics_session_closed(long serial)
{
    if (serial > 0)
        yz_movie_end_live_draw_mode((LONG)serial);
}

static void yz_play_queued_movie(const char* path, LONG serial)
{
    if (!rsx_live_draw_enabled() || !movie_ffmpeg_available())
        return;

    /* The movie owns graphics from the moment host decode begins.  AIX/video
     * preparation writes guest-backed pages which the outgoing RSX frame can
     * also reference, so no live-draw section may straddle that window.  Take
     * the FIFO-serialized movie handoff before opening the decoder. */
    yz_movie_begin_live_draw_mode(serial);
    MoviePlayer* mv = movie_open(path);
    if (!mv) {
        fprintf(stderr, "[movie] unable to decode queued movie '%s'\n", path);
        yz_movie_end_live_draw_mode(serial);
        yz_movie_complete(serial);
        return;
    }

    const uint32_t w = (uint32_t)movie_width(mv);
    const uint32_t h = (uint32_t)movie_height(mv);
    int fps = (int)(movie_framerate(mv) + 0.5);
    if (fps <= 0) fps = 30;
    /* Decode the first picture before starting the audio clock. MPEG frame
     * startup can cost roughly one frame, which otherwise makes audio lead
     * from the first visible image even though steady-state cadence is sound. */
    double first_pts = 0.0;
    const uint8_t* first_rgba = movie_next_rgba(mv, &first_pts);
    if (!first_rgba) {
        fprintf(stderr, "[movie] no video frames in '%s'\n", path);
        fflush(stderr);
        movie_close(mv);
        yz_movie_end_live_draw_mode(serial);
        yz_movie_complete(serial);
        return;
    }
    /* The movie's own audio replaces the game's audio ports while it plays. */
    const int host_audio =
        movie_has_audio(mv) &&
        cellAudioHostStreamStart(movie_audio_s16(mv),
                                 movie_audio_frames(mv), 1) == 0;
    const ULONGLONG wall_start_ms = GetTickCount64();
    int frames = 0;
    int superseded = 0;
    int cancelled = 0;
    int skip_requested = 0;
    int skip_guest_acked = 0;
    int natural_completion = 0;

    InterlockedExchange(&g_movie_present_timescale, fps);
    InterlockedExchange(&g_movie_presenting_serial, serial);
    fprintf(stderr,
            "[movie] presenting '%s' (%ux%u @ %d fps, audio=%s)\n",
            path, w, h, fps, host_audio ? "FFmpeg ADX" : "native/none");
    fflush(stderr);
    cellPad_host_movie_skip_begin();
    for (;;) {
        if (InterlockedCompareExchange(&g_movie_open_serial, 0, 0) != serial) {
            superseded = 1;
            break;
        }
        if (InterlockedCompareExchange(&g_movie_cancelled_serial, 0, 0) >= serial) {
            cancelled = 1;
            break;
        }
        if (cellPad_host_movie_skip_requested()) {
            /* The host and guest observe the same physical Start press.  The
             * host edge is diagnostic only: whether this particular movie is
             * skippable belongs to the guest player.  Keep decoding until the
             * guest's Stop/Close hook requests cancellation; otherwise an
             * ignored or too-early Start must leave playback running. */
            skip_requested = 1;
            fprintf(stderr,
                    "[movie] Start observed; awaiting guest Stop/Close serial=%ld\n",
                    serial);
            fflush(stderr);
        }
        if (skip_requested && !skip_guest_acked &&
            cellPad_host_movie_skip_guest_seen()) {
            skip_guest_acked = 1;
            fprintf(stderr,
                    "[movie] guest received Start serial=%ld\n",
                    serial);
            fflush(stderr);
        }
        double pts = 0.0;
        const uint8_t* rgba;
        if (frames == 0) {
            rgba = first_rgba;
            pts = first_pts;
        } else {
            rgba = movie_next_rgba(mv, &pts);
        }
        if (!rgba) {
            natural_completion = 1;
            fprintf(stderr,
                    "[movie] host EOS; publishing PLAYEND to mwPly owner serial=%ld\n",
                    serial);
            fflush(stderr);
            break;
        }
        const double target = pts > 0.0 ? pts : (double)frames / fps;
        for (;;) {
            const uint64_t audio_cursor =
                host_audio ? cellAudioHostStreamPositionFrames() : 0;
            const int audio_finished =
                host_audio ? cellAudioHostStreamFinished() : 0;
            const double wall_clock =
                (GetTickCount64() - wall_start_ms) / 1000.0;
            /* Audio is authoritative while samples are still playing. Some
             * clips (notably Sega) intentionally have a silent video tail;
             * after audio EOS, continue that tail on the same wall clock. */
            const double clock = host_audio && !audio_finished
                ? (double)audio_cursor / 48000.0
                : wall_clock;
            if (clock + 0.001 >= target) break;
            if (InterlockedCompareExchange(&g_movie_cancelled_serial, 0, 0) >= serial) {
                cancelled = 1;
                break;
            }
            if (rsx_null_backend_pump_messages() < 0) {
                cancelled = 1;
                break;
            }
            Sleep(1);
        }
        if (cancelled) break;
        rsx_live_draw_present_rgba(rgba, w, h);
        frames++;
        InterlockedExchange(&g_movie_presented_frames, frames);
        if (rsx_null_backend_pump_messages() < 0) break;
    }
    if (host_audio)
        cellAudioHostStreamStop();
    InterlockedCompareExchange(&g_movie_presenting_serial, 0, serial);
    /* Graphics ownership lasts until the guest's Destroy closes this exact
     * session (yz_host_movie_graphics_session_closed), not until host
     * presentation ends. */
    movie_close(mv);
    /* The host movie is an overlay, not an RSX FIFO producer: it produces no
     * flip.  The guest's own flip/event path retires its work; the sticky
     * completion flag is published afterwards. */
    yz_movie_complete(serial);
    cellPad_host_movie_skip_end();
    if (skip_requested && !cancelled && !superseded && !natural_completion) {
        fprintf(stderr,
                "[movie] guest did not accept skip; completed naturally serial=%ld\n",
                serial);
    }
    fprintf(stderr, "[movie] %s after %d frames\n",
            superseded ? "switched streams" :
            cancelled ? "cancelled cleanly" : "presentation complete",
            frames);
    fflush(stderr);
}

/* Dedicated window thread. A Win32 window must be created and message-pumped on
 * the same thread, so it lives here rather than on the main/consumer threads.
 * rsx_null_backend_init() opens the window and registers the null backend (GDI
 * clear-color present); the consumer + flip path then drive it.  When the live
 * D3D12 renderer is enabled it binds a swap chain to that same HWND and takes
 * over presentation. */
static DWORD WINAPI yz_window_thread(LPVOID)
{
    yz_thread_adopt_host("window");
    if (rsx_null_backend_init(1280, 720, "Yakuza: Dead Souls (ps3recomp)") != 0) {
        fprintf(stderr, "[rsx] window init failed\n");
        return 1;
    }
    if (rsx_live_draw_enabled()) {
        int r = rsx_live_draw_init(rsx_null_backend_get_hwnd(), 1280, 720,
                                   yz_rsx_live_guest_ptr, nullptr);
        if (r == 0) {
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12); null GDI present suppressed\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED (%d) -> falling back to null present\n", r);
        }
    }

    cellfs_set_open_hook(yz_auth_open_hook);

    for (;;) {
        char queued_movie[MAX_PATH] = {};
        LONG queued_serial = 0;
        AcquireSRWLockShared(&g_movie_open_lock);
        queued_serial = g_movie_open_serial;
        if (queued_serial != g_movie_played_serial)
            strncpy(queued_movie, g_movie_open_path, sizeof(queued_movie) - 1);
        ReleaseSRWLockShared(&g_movie_open_lock);

        if (queued_movie[0] && queued_serial != g_movie_played_serial) {
            g_movie_played_serial = queued_serial;
            yz_play_queued_movie(queued_movie, queued_serial);
        }
        if (rsx_null_backend_pump_messages() < 0)
            break;            /* window closed */
        Sleep(8);
    }
    /* Closing the window ends the process: the guest main thread has no Win32
     * close event and would otherwise keep audio/worker threads alive with no
     * window.  Take the FIFO lock first so no consumer step is mid-flight
     * during teardown. */
    yz_rsx_fifo_acquire();
    fprintf(stderr, "[window] closed; terminating host process\n");
    fflush(stderr);
    ExitProcess(0);
}
