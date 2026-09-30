/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Owns the Win32 window, counts flips, forwards keyboard events to cellPad
 * and updates the title statistics; rendering is done by rsx_live_draw. When
 * no GPU renderer owns the window it paints the RSX clear color with GDI.
 */

#include "rsx_null_backend.h"
#include "rsx_live_draw.h"
#include "../input/cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* Current clear color (ARGB -> COLORREF) */
    COLORREF clear_color;

    /* Frame counter */
    volatile LONG64 frame_count;
    u64      last_fps_time;
    u64      frames_at_mark;
    u32      fps;
    int      title_ui_timer;

    int      window_closed;
} NullBackendState;

static NullBackendState s_state;

static u64 null_frame_count(void)
{
    return (u64)InterlockedCompareExchange64(&s_state.frame_count, 0, 0);
}

#define NULL_TITLE_TIMER_ID 0x4A31u
static void null_update_title(void);

static int null_start_title_timer(HWND hwnd)
{
    return SetTimer(hwnd, NULL_TITLE_TIMER_ID, 1000u, NULL) != 0;
}

/* When a GPU backend (live draw) owns a flip-model swap chain on our
 * HWND, GDI painting corrupts the swap-chain surface -- suppress both. */
static int s_present_suppressed = 0;

/* ---------------------------------------------------------------------------
 * Win32 window procedure
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK null_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == NULL_TITLE_TIMER_ID && s_state.title_ui_timer) {
            null_update_title();
            return 0;
        }
        break;
    case WM_CLOSE:
        s_state.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, NULL_TITLE_TIMER_ID);
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
                     s_state.fps, (unsigned long long)null_frame_count());
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
    case WM_SYSKEYDOWN:
        cellPad_host_key_event((u32)wp, 1);
        if (wp == VK_ESCAPE) {
            s_state.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        cellPad_host_key_event((u32)wp, 0);
        return 0;
    case WM_KILLFOCUS:
        cellPad_host_key_reset();
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
    (void)width;
    (void)height;
    return 0;
}

static void null_shutdown(void* ud)
{
    (void)ud;
}

static void null_begin_frame(void* ud)
{
    (void)ud;
}

static void null_update_title(void)
{
    /* Refresh the window title at most once per second. */
    const u64 frames = null_frame_count();
    s_state.fps = (u32)(frames - s_state.frames_at_mark);
    s_state.frames_at_mark = frames;
    s_state.last_fps_time = GetTickCount64();
    if (s_state.hwnd) {
        char title[224];
        snprintf(title, sizeof(title),
                 "Yakuza: Dead Souls  |  flips %llu  |  frames %u  |  draws %u  |  %.2f presented FPS",
                 (unsigned long long)frames,
                 rsx_live_draw_get_frames(),
                 rsx_live_draw_get_last_draws(),
                 rsx_live_draw_get_present_fps());
        SetWindowTextA(s_state.hwnd, title);
    }
}

static void null_end_frame(void* ud)
{
    (void)ud;
    InterlockedIncrement64(&s_state.frame_count);
    /* A cross-thread SetWindowText sends synchronously to the UI thread.
     * A window timer normally owns all title work on the UI thread, so a
     * delayed message pump cannot hold up flip retirement or the next host
     * vblank tick; this path only runs if the timer could not be created. */
    if (!s_state.title_ui_timer &&
        GetTickCount64() - s_state.last_fps_time >= 1000u)
        null_update_title();
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
    .set_render_target = NULL,
    .set_viewport      = NULL,
    .set_blend         = NULL,
    .set_depth_stencil = NULL,
    .clear             = null_clear,
    .draw_arrays       = NULL,
    .draw_indexed      = NULL,
    .bind_texture      = NULL,
};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_null_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_state, 0, sizeof(s_state));
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
    /*
     * CreateWindowEx(... WS_VISIBLE ...) performs the process's first show
     * request, which Windows may replace with STARTUPINFO.wShowWindow; a
     * launch through a hidden parent would inherit SW_HIDE.  A second show
     * request is authoritative.
     */
    ShowWindow(s_state.hwnd, SW_SHOW);
    UpdateWindow(s_state.hwnd);
    {   /* WM_SETICON beats the shell's per-app taskbar icon cache. */
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
    s_state.title_ui_timer = null_start_title_timer(s_state.hwnd);
    if (!s_state.title_ui_timer)
        fprintf(stderr, "[window-title] timer creation failed: %lu; falling back to flip-thread updates\n",
                GetLastError());

    /* Register as the active RSX backend */
    rsx_set_backend(&s_null_backend);
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
int rsx_null_backend_graphics_suppressed(void)
{
    return s_present_suppressed && rsx_get_backend()==&s_null_backend;
}

#else /* !_WIN32 */

/* Not supported on this platform. */

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
int rsx_null_backend_graphics_suppressed(void) { return 0; }

#endif /* _WIN32 */
