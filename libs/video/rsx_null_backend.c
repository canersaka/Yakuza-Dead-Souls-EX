/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Implements rsx_backend callbacks using a simple Win32 GDI window.
 * No GPU rendering — just clears to the RSX clear color and presents.
 * Used for debugging command buffer flow before a real backend exists.
 */

#include "rsx_null_backend.h"
#include <stdio.h>
#include <string.h>

/* Real game-frame counter from the Track B live-draw engine (rsx_live_draw.c).
 * Lets the title show frames-actually-rendered next to the raw flip/present
 * count, so a stall (frames frozen while flips keep climbing) is visible. */
extern u32 rsx_live_draw_get_frames(void);
extern u32 rsx_live_draw_get_last_draws(void);

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    HWND     hwnd;
    HDC      hdc;
    u32      width;
    u32      height;

    /* Current clear color (ARGB -> COLORREF) */
    COLORREF clear_color;

    /* Frame counter */
    u64      frame_count;
    u64      last_fps_time;
    u32      fps;

    int      window_closed;
} NullBackendState;

static NullBackendState s_state;

/* When a GPU backend (Track B live draw) owns a flip-model swap chain on our
 * HWND, GDI painting corrupts the swap-chain surface -- suppress both. */
static int s_present_suppressed = 0;

/* ---------------------------------------------------------------------------
 * Win32 window procedure
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK null_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_state.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* When a GPU backend owns the surface, validate the paint but draw
         * nothing so GDI does not overwrite the flip-model swap chain. */
        if (!s_present_suppressed) {
            /* Fill with current clear color */
            HBRUSH brush = CreateSolidBrush(s_state.clear_color);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, brush);
            DeleteObject(brush);

            /* Draw debug overlay text */
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            char buf[128];
            snprintf(buf, sizeof(buf), "ps3recomp null backend | %u FPS | frame %llu",
                     s_state.fps, (unsigned long long)s_state.frame_count);
            TextOutA(hdc, 10, 10, buf, (int)strlen(buf));

            char buf2[64];
            snprintf(buf2, sizeof(buf2), "Clear: #%06X",
                     (unsigned)(s_state.clear_color & 0xFFFFFF));
            TextOutA(hdc, 10, 30, buf2, (int)strlen(buf2));
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s_state.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------------------
 * Backend callbacks
 * -----------------------------------------------------------------------*/

static int null_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[RSX null] init(%ux%u)\n", width, height);
    return 0;
}

static void null_shutdown(void* ud)
{
    (void)ud;
    printf("[RSX null] shutdown\n");
}

static void null_begin_frame(void* ud)
{
    (void)ud;
}

static void null_end_frame(void* ud)
{
    (void)ud;
    s_state.frame_count++;

    /* Real FPS: frames counted in the last ~1 s window. */
    static u64 frames_at_mark = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_state.last_fps_time >= 1000) {
        s_state.fps = (u32)(s_state.frame_count - frames_at_mark);
        frames_at_mark = s_state.frame_count;
        s_state.last_fps_time = now;
    }

    /* Flip count + FPS live in the window title so they show even when the GPU
     * backend owns the surface and the GDI overlay is suppressed. The internal
     * renderer label (live-draw D3D12 vs GDI fallback) stays out of the title
     * (user request 2026-07-10); it is still visible in the boot log banner. */
    if (s_state.hwnd) {
        char title[192];
        snprintf(title, sizeof(title),
                 "Yakuza: Dead Souls  |  flips %llu  |  frames %u  |  draws %u  |  %u FPS",
                 (unsigned long long)s_state.frame_count,
                 rsx_live_draw_get_frames(), rsx_live_draw_get_last_draws(),
                 s_state.fps);
        SetWindowTextA(s_state.hwnd, title);
    }
}

static void null_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (s_present_suppressed)
        return;                 /* GPU backend owns presentation */

    /* Trigger a repaint */
    if (s_state.hwnd)
        InvalidateRect(s_state.hwnd, NULL, FALSE);
}

static void null_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB to Win32 COLORREF (BGR) */
    u8 r = (color >> 16) & 0xFF;
    u8 g = (color >> 8) & 0xFF;
    u8 b = color & 0xFF;
    s_state.clear_color = RGB(r, g, b);
}

static void null_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    printf("[RSX null] set_render_target(format=0x%X, %ux%u)\n",
           state->surface_format, state->surface_clip_w, state->surface_clip_h);
}

static void null_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    printf("[RSX null] set_viewport(%u,%u %ux%u)\n",
           state->viewport_x, state->viewport_y,
           state->viewport_w, state->viewport_h);
}

static void null_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_arrays(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        s_draw_log_count++;
    }
}

static void null_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static int s_draw_log_count = 0;
    if (s_draw_log_count < 20) {
        printf("[RSX null] draw_indexed(prim=%u, offset=%u, count=%u)\n",
               primitive, offset, count);
        s_draw_log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_null_backend = {
    .userdata          = &s_state,
    .init              = null_init,
    .shutdown          = null_shutdown,
    .begin_frame       = null_begin_frame,
    .end_frame         = null_end_frame,
    .present           = null_present,
    .set_render_target = null_set_render_target,
    .set_viewport      = null_set_viewport,
    .set_blend         = NULL,
    .set_depth_stencil = NULL,
    .clear             = null_clear,
    .draw_arrays       = null_draw_arrays,
    .draw_indexed      = null_draw_indexed,
    .bind_texture      = NULL,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.width = width;
    s_state.height = height;
    s_state.clear_color = RGB(0, 0, 64); /* dark blue default */

    /* Register window class */
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = null_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    /* Host exe's embedded icon (resource id 1) for taskbar/alt-tab; NULL
     * (default) when the game project embeds none. */
    wc.hIcon = LoadIconA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = "ps3recomp_null";
    RegisterClassExA(&wc);

    /* Compute window size from client area */
    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    s_state.hwnd = CreateWindowExA(
        0, "ps3recomp_null",
        title ? title : "ps3recomp",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    if (!s_state.hwnd) {
        printf("[RSX null] ERROR: CreateWindow failed (%lu)\n", GetLastError());
        return -1;
    }
    {   /* WM_SETICON beats the shell's per-app taskbar icon cache (see the
         * d3d12 backend's create_window). */
        HICON big = (HICON)LoadImageA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1),
                                      IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), 0);
        HICON sml = (HICON)LoadImageA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1),
                                      IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON), 0);
        if (big) SendMessageA(s_state.hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (sml) SendMessageA(s_state.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sml);
    }

    s_state.hdc = GetDC(s_state.hwnd);
    s_state.last_fps_time = GetTickCount64();

    /* Register as the active RSX backend */
    rsx_set_backend(&s_null_backend);

    printf("[RSX null] Window created: %ux%u\n", width, height);
    return 0;
}

void rsx_null_backend_shutdown(void)
{
    if (s_state.hwnd) {
        ReleaseDC(s_state.hwnd, s_state.hdc);
        DestroyWindow(s_state.hwnd);
        s_state.hwnd = NULL;
    }
    rsx_set_backend(NULL);
    printf("[RSX null] Backend shut down after %llu frames\n",
           (unsigned long long)s_state.frame_count);
}

int rsx_null_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_state.window_closed ? -1 : 0;
}

void* rsx_null_backend_get_hwnd(void)
{
    return (void*)s_state.hwnd;
}

void rsx_null_backend_suppress_present(int on)
{
    s_present_suppressed = on;
}

#else /* !_WIN32 */

/* Stub for non-Windows platforms — TODO: SDL2 or X11 backend */

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    (void)width; (void)height; (void)title;
    printf("[RSX null] Window backend not available on this platform\n");
    return -1;
}

void rsx_null_backend_shutdown(void) {}

int rsx_null_backend_pump_messages(void)
{
    return 0;
}

void* rsx_null_backend_get_hwnd(void) { return 0; }
void  rsx_null_backend_suppress_present(int on) { (void)on; }

#endif /* _WIN32 */
