/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "cellPad.h"
#include "ps3emu/endian.h"   /* ps3_bswap16/32: CellPadData/Info2 are guest big-endian */
#include "ps3emu/yz_runtime_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
static volatile long s_accepted_input_serial;
/* Opt-in unattended gameplay proof.  The renderer acknowledges each capture
 * boundary by advancing this phase, so input cannot race the "before" image.
 * Zero in ordinary builds/runs; YZ_MOVEMENT_PROOF is required to arm it. */
volatile long g_yz_movement_proof_phase;
/* Renderer-owned semantic state for the unavoidable post-movement authored
 * dialogue.  The pad route remains idle until the HUD disappears, advances
 * that dialogue, and stops permanently after free gameplay is stable again. */
volatile long g_yz_movement_post_dialogue_seen;
volatile long g_yz_movement_gameplay_returned;
volatile long g_yz_movement_stable_gameplay;
volatile long g_yz_movement_proof_leg = 1;
/* Boundary-only diagnostic owned by the recompiled title.  It snapshots the
 * game's cached input object and camera producer without adding per-poll log
 * traffic to the timing-sensitive pad path. */
void yz_movement_frontier_snapshot(const char* reason);
#if PAD_BACKEND_XINPUT
static volatile LONG s_window_key_state[256];
#endif
static int           s_movie_skip_down = 0;
#if PAD_BACKEND_XINPUT
static volatile LONG s_movie_skip_active = 0;
static volatile LONG s_movie_skip_guest_seen = 0;
static volatile LONG s_movie_skip_poll_serial = 0;
static volatile LONG s_movie_skip_seen_poll = 0;
#else
static volatile int  s_movie_skip_active = 0;
static volatile int  s_movie_skip_guest_seen = 0;
static volatile int  s_movie_skip_poll_serial = 0;
static volatile int  s_movie_skip_seen_poll = 0;
#endif

static int pad_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_PAD_TRACE") ? 1 : 0;
    return enabled;
}

#if PAD_BACKEND_XINPUT
static int pad_movement_proof_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_MOVEMENT_PROOF") ? 1 : 0;
    return enabled;
}
#endif

static u32 pad_connected_mask(void)
{
    u32 mask = 0;
    for (u32 i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected) mask |= 1u << i;
    return mask;
}

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y is inverted vs XInput (up = 0): reflect about 128
         * (256 - x), NOT 255 - x — the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START in the digital mask. Safe because
         * pad_xinput_stick_to_u8 returns 128 +/- 127 (never 0), so no u8 wrap. */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static int pad_keyboard_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("YZ_NO_KEYBOARD_PAD") ? 0 : 1;
    return enabled;
}

static int pad_key_down(int virtual_key)
{
    const int message_down =
        virtual_key >= 0 && virtual_key < 256 &&
        InterlockedCompareExchange(&s_window_key_state[virtual_key], 0, 0) != 0;
    return message_down || (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

static int pad_window_key_down(int virtual_key)
{
    return virtual_key >= 0 && virtual_key < 256 &&
        InterlockedCompareExchange(&s_window_key_state[virtual_key], 0, 0) != 0;
}

static int pad_window_key_any_down(void)
{
    for (unsigned i = 0; i < 256; ++i)
        if (InterlockedCompareExchange(&s_window_key_state[i], 0, 0) != 0)
            return 1;
    return 0;
}

void cellPad_host_key_event(u32 virtual_key, int down)
{
    if (virtual_key < 256)
        InterlockedExchange(&s_window_key_state[virtual_key], down ? 1 : 0);
}

void cellPad_host_key_reset(void)
{
    for (unsigned i = 0; i < 256; ++i)
        InterlockedExchange(&s_window_key_state[i], 0);
}

static int pad_keyboard_window_focused(void)
{
    HWND foreground = GetForegroundWindow();
    DWORD foreground_pid = 0;
    if (!foreground)
        return 0;
    GetWindowThreadProcessId(foreground, &foreground_pid);
    return foreground_pid == GetCurrentProcessId();
}

static int pad_host_start_down(void)
{
    int down = 0;
    for (DWORD i = 0; i < PAD_MAX_HOST_PORTS; ++i) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));
        if (XInputGetState(i, &state) == ERROR_SUCCESS &&
            (state.Gamepad.wButtons & XINPUT_GAMEPAD_START))
            down = 1;
    }
    if (pad_keyboard_enabled() &&
        ((pad_keyboard_window_focused() && pad_key_down(VK_RETURN)) ||
         pad_window_key_down(VK_RETURN)))
        down = 1;
    return down;
}

/* Merge a keyboard-backed virtual controller into port 0.  This deliberately
 * runs after XInput so a real controller and the keyboard can be used at the
 * same time.  The keyboard also keeps port 0 connected when no XInput device
 * is present, which lets title/menu input work without a controller. */
static void pad_merge_keyboard(void)
{
    if (!pad_keyboard_enabled())
        return;

    PadHostState* hs = &s_host_state[0];
    if (!hs->connected) {
        memset(hs, 0, sizeof(*hs));
        hs->analog_lx = 128;
        hs->analog_ly = 128;
        hs->analog_rx = 128;
        hs->analog_ry = 128;
    }
    hs->connected = 1;

    /* GetAsyncKeyState is system-wide. Ignore it unless a process window is
     * foreground so background key state cannot reach guest input. The
     * diagnostic unattended route below is process-local, not keyboard state,
     * so it must remain eligible without a foreground window. */
    {
        extern volatile unsigned long long g_yz_auto_start_tick;
        if (!pad_keyboard_window_focused() && !pad_window_key_any_down() &&
            !(g_yz_runtime_config.auto_new_game &&
              g_yz_auto_start_tick))
            return;
    }

    u16 btns = hs->buttons;
    if (pad_key_down(VK_BACK))   btns |= CELL_PAD_CTRL_SELECT;
    if (pad_key_down(VK_RETURN)) btns |= CELL_PAD_CTRL_START;
    if (pad_key_down(VK_UP))     btns |= CELL_PAD_CTRL_UP;
    if (pad_key_down(VK_RIGHT))  btns |= CELL_PAD_CTRL_RIGHT;
    if (pad_key_down(VK_DOWN))   btns |= CELL_PAD_CTRL_DOWN;
    if (pad_key_down(VK_LEFT))   btns |= CELL_PAD_CTRL_LEFT;
    if (pad_key_down('1'))       btns |= CELL_PAD_CTRL_L2;
    if (pad_key_down('3'))       btns |= CELL_PAD_CTRL_R2;
    if (pad_key_down('Q'))       btns |= CELL_PAD_CTRL_L1;
    if (pad_key_down('E'))       btns |= CELL_PAD_CTRL_R1;
    if (pad_key_down('V') || pad_key_down('I')) btns |= CELL_PAD_CTRL_TRIANGLE;
    if (pad_key_down('C') || pad_key_down('L')) btns |= CELL_PAD_CTRL_CIRCLE;
    if (pad_key_down('X') || pad_key_down('K')) btns |= CELL_PAD_CTRL_CROSS;
    if (pad_key_down('Z') || pad_key_down('J')) btns |= CELL_PAD_CTRL_SQUARE;
    hs->buttons = btns;

    /* WASD supplies a full-strength left stick while arrows remain a D-pad. */
    if (pad_key_down('A')) hs->analog_lx = 0;
    if (pad_key_down('D')) hs->analog_lx = 255;
    if (pad_key_down('W')) hs->analog_ly = 0;
    if (pad_key_down('S')) hs->analog_ly = 255;

    if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
    if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    hs->press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
    hs->press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    hs->press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    hs->press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    hs->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    hs->press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    hs->press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    hs->press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    hs->press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
    hs->press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;

    /* Unattended a010 acceptance route. YZ_AUTO_NEW_GAME is diagnostic-only:
     * after YZ_AUTO_START fires at the authoritative title callback, hold
     * Confirm long enough for slow frames to observe it, then release it.
     * Stop once a010 becomes active so scripted input cannot affect the scene
     * itself. */
    {
        extern volatile unsigned long long g_yz_auto_start_tick;
        extern volatile long g_yz_a010_root_active;
        extern volatile long g_yz_auto_new_game_complete;
        static u16 prior_auto_accept = 0;
        static int load_stop_configured;
        static unsigned long long next_load_stop_poll;
        static char load_confirm_stop_file[MAX_PATH * 2];
        /* s_host_state is persistent when the keyboard is the only connected
         * controller.  Remove our previous synthetic bit before evaluating
         * this poll, otherwise the first "pulse" latches as a held button and
         * the game never observes another rising edge. */
        if (prior_auto_accept) {
            hs->buttons &= (u16)~prior_auto_accept;
            if (prior_auto_accept == CELL_PAD_CTRL_CIRCLE)
                hs->press_circle = 0;
            else if (prior_auto_accept == CELL_PAD_CTRL_CROSS)
                hs->press_cross = 0;
            else if (prior_auto_accept == CELL_PAD_CTRL_UP)
                hs->press_up = 0;
            prior_auto_accept = 0;
        }
        if (g_yz_a010_root_active)
            InterlockedExchange(&g_yz_auto_new_game_complete, 1);
        if (!load_stop_configured) {
            const char* path = getenv("YZ_AUTO_LOAD_GAME_CONFIRM_STOP_FILE");
            load_stop_configured = 1;
            if (path && *path) {
                strncpy(load_confirm_stop_file, path,
                        sizeof(load_confirm_stop_file) - 1u);
                load_confirm_stop_file[
                    sizeof(load_confirm_stop_file) - 1u] = '\0';
            }
        }
        if (load_confirm_stop_file[0] &&
            !InterlockedCompareExchange(
                &g_yz_auto_new_game_complete, 0, 0)) {
            const unsigned long long now = GetTickCount64();
            if (now >= next_load_stop_poll) {
                next_load_stop_poll = now + 100ull;
                if (GetFileAttributesA(load_confirm_stop_file) !=
                        INVALID_FILE_ATTRIBUTES) {
                    InterlockedExchange(&g_yz_auto_new_game_complete, 1);
                    fprintf(stderr,
                            "[auto-load-game] menu/save Confirm input "
                            "stopped after exact save load\n");
                    fflush(stderr);
                }
            }
        }
        if (g_yz_runtime_config.auto_new_game &&
            g_yz_auto_start_tick &&
            !InterlockedCompareExchange(
                &g_yz_auto_new_game_complete, 0, 0)) {
            const unsigned long long elapsed =
                GetTickCount64() - g_yz_auto_start_tick;
            const unsigned long long period = 3000u;
            /* The unattended acceptance route must be state-driven: a slow
             * menu can miss every one of the old five pulses even though the
             * same cadence succeeds on another boot.  Keep trying until the
             * a010 root proves New Game was accepted.  Ordinary/manual runs
             * retain the bounded five-pulse handoff below. */
            const unsigned attempts = getenv("YZ_A010_ACCEPT_FAST") ? 60u : 5u;
            /*
             * The main-menu hook first moves Load Game -> New Game and accepts
             * it through the game's cached input.  Do not begin the generic
             * confirmation-screen pulses until that operation has completed;
             * an earlier pulse selects Load Game while save data is present.
             */
            const unsigned long long first = 12000u;
            if (elapsed >= first && elapsed < first + period * attempts) {
                const unsigned pulse = (unsigned)((elapsed - first) / period);
                const unsigned long long phase = (elapsed - first) % period;
                static unsigned long long logged = 0;
                const u16 accept = g_yz_runtime_config.auto_new_game_circle
                    ? CELL_PAD_CTRL_CIRCLE : CELL_PAD_CTRL_CROSS;
                if (phase < 2000u) {
                    hs->buttons |= accept;
                    prior_auto_accept = accept;
                    if (g_yz_runtime_config.auto_new_game_circle)
                        hs->press_circle = 255;
                    else
                        hs->press_cross = 255;
                    if (pulse < 64u && !(logged & (1ull << pulse))) {
                        logged |= 1ull << pulse;
                        fprintf(stderr,
                                "[auto-new-game] %s pulse %u/%u at +%llums\n",
                                g_yz_runtime_config.auto_new_game_circle
                                    ? "Circle" : "Cross",
                                pulse + 1u, attempts, elapsed);
                        fflush(stderr);
                    }
                }
            }
        }
    }

    /* Dedicated unattended route to the first Akiyama/Hana dialogue.  New
     * Game remains owned by the state-driven Confirm route above.  Once the
     * a010 root proves gameplay has started, pulse Start (never Confirm) to
     * skip authored cutscenes.  The external visual classifier creates the
     * exact stop file after it has recognized the first X-to-continue
     * dialogue; checking that file at a bounded cadence keeps filesystem I/O
     * out of the pad hot path and makes the route permanently quiescent at
     * the measurement checkpoint. */
    {
        extern volatile long g_yz_a010_root_active;
        static int configured;
        static int route_started;
        static int route_stopped;
        static u16 prior_route_start;
        static unsigned long long route_input_tick;
        static unsigned long long route_input_delay_ms;
        static unsigned long long next_stop_poll;
        static unsigned long long next_load_start_poll;
        static unsigned long long load_start_tick;
        static int load_game_route;
        static int load_start_armed;
        static int load_start_complete;
        static char stop_file[MAX_PATH * 2];
        static char load_start_file[MAX_PATH * 2];

        if (prior_route_start) {
            hs->buttons &= (u16)~CELL_PAD_CTRL_START;
            prior_route_start = 0;
        }
        if (g_yz_runtime_config.akiyama_dialogue_route && !configured) {
            const char* path = getenv("YZ_AKIYAMA_DIALOGUE_STOP_FILE");
            const char* delay = getenv("YZ_AKIYAMA_ROUTE_START_DELAY_MS");
            const char* start_path = getenv("YZ_AUTO_LOAD_GAME_START_FILE");
            configured = 1;
            load_game_route = getenv("YZ_AUTO_LOAD_GAME") != NULL;
            if (delay && *delay) {
                route_input_delay_ms = _strtoui64(delay, NULL, 10);
                if (route_input_delay_ms > 120000ull)
                    route_input_delay_ms = 120000ull;
            }
            if (path && *path) {
                strncpy(stop_file, path, sizeof(stop_file) - 1u);
                stop_file[sizeof(stop_file) - 1u] = '\0';
            } else {
                route_stopped = 1;
                fprintf(stderr,
                        "[akiyama-route] disabled: stop file is required\n");
                fflush(stderr);
            }
            if (load_game_route) {
                if (start_path && *start_path) {
                    strncpy(load_start_file, start_path,
                            sizeof(load_start_file) - 1u);
                    load_start_file[sizeof(load_start_file) - 1u] = '\0';
                } else {
                    route_stopped = 1;
                    fprintf(stderr,
                            "[auto-load-game] disabled: one-shot Start arm "
                            "file is required\n");
                    fflush(stderr);
                }
            }
        }
        if (g_yz_runtime_config.akiyama_dialogue_route &&
            !route_stopped) {
            const unsigned long long now = GetTickCount64();
            /* New Game waits for the a010 gameplay root.  The saved-game
             * route is instead armed from process start and remains inert
             * until the external visual oracle creates its one-shot Start
             * file; this avoids assuming the loaded Chapter-2 scene uses the
             * same a010 root as the opening route. */
            if (!route_started &&
                (g_yz_a010_root_active || load_game_route)) {
                route_started = 1;
                route_input_tick = now + route_input_delay_ms;
                next_stop_poll = now;
                fprintf(stderr,
                        "[%s] Start-only cutscene route armed delay_ms=%llu\n",
                        load_game_route ? "auto-load-game" : "akiyama-route",
                        route_input_delay_ms);
                fflush(stderr);
            }
            if (route_started && now >= next_stop_poll) {
                const DWORD attrs = GetFileAttributesA(stop_file);
                next_stop_poll = now + 250ull;
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    route_stopped = 1;
                    fprintf(stderr,
                            "[akiyama-route] input stopped at visual checkpoint\n");
                    fflush(stderr);
                }
            }
            if (route_started && !route_stopped && !load_game_route &&
                now >= route_input_tick) {
                const unsigned long long phase =
                    (now - route_input_tick) % 8000ull;
                if (phase < 250ull) {
                    hs->buttons |= CELL_PAD_CTRL_START;
                    prior_route_start = CELL_PAD_CTRL_START;
                }
            }
            if (route_started && !route_stopped && load_game_route &&
                !load_start_complete) {
                if (!load_start_armed && now >= next_load_start_poll) {
                    next_load_start_poll = now + 100ull;
                    if (GetFileAttributesA(load_start_file) !=
                            INVALID_FILE_ATTRIBUTES) {
                        load_start_armed = 1;
                        load_start_tick = now;
                        fprintf(stderr,
                                "[auto-load-game] positively gated cutscene; "
                                "one Start edge armed\n");
                        fflush(stderr);
                    }
                }
                if (load_start_armed && now - load_start_tick < 750ull) {
                    hs->buttons |= CELL_PAD_CTRL_START;
                    prior_route_start = CELL_PAD_CTRL_START;
                } else if (load_start_armed) {
                    load_start_complete = 1;
                    fprintf(stderr,
                            "[auto-load-game] one-shot Start released; no "
                            "further synthetic cutscene input\n");
                    fflush(stderr);
                }
            }
        }
    }

    /* Headless-safe acceptance controller for the promotion proof.  Desktop
     * key injection cannot reach a deliberately hidden D3D window, so drive
     * the same guest-visible PadHostState used by real XInput/keyboard input.
     *
     * Phase ownership is deliberately split:
     *   input:    0 -> 1, 2 -> 3, 4 -> 5
     *   renderer: 1 -> 2, 3 -> 4, 5 -> 6
     * This handshake proves that the pre-movement surface was captured before
     * the first stick sample, then sustains movement for a measured,
     * configurable interval before the paired image is taken.  Phase 6
     * resumes Confirm pulses and
     * accepts numbered arm files so an unattended route can cross multiple
     * gameplay/dialogue boundaries without another build or restart. */
    if (pad_movement_proof_enabled()) {
        extern volatile unsigned long long g_yz_auto_start_tick;
        static unsigned long long movement_tick = 0;
        static unsigned long long last_logged_pulse = ~0ull;
        static unsigned long long next_arm_file_poll = 0;
        static unsigned long long next_route_arm_file_poll = 0;
        static int arm_file_mode = -1;
        static int dialogue_arm_file_mode;
        static int dialogue_arm_ready;
        static unsigned long long next_dialogue_arm_poll;
        static unsigned long long dialogue_period_ms;
        static unsigned long long dialogue_hold_ms;
        static int dialogue_timing_configured;
        static int skip_camera = -1;
        static unsigned movement_leg = 1;
        static unsigned max_movement_legs;
        static int max_movement_legs_configured;
        static unsigned long long movement_hold_ms;
        static unsigned long long final_movement_hold_ms;
        static int movement_hold_configured;
        static u8 movement_lx;
        static u8 movement_ly;
        static int route_complete_logged;
        static int route_wait_logged;
        static unsigned long long post_dialogue_tick = 0;
        static unsigned long long last_post_logged_pulse = ~0ull;
        static int next_arm_path_logged = 0;
        static char arm_file[MAX_PATH * 2];
        static char dialogue_arm_file[MAX_PATH * 2];
        static char next_arm_file[MAX_PATH * 2];
        const unsigned long long now = GetTickCount64();
        const unsigned long long elapsed = g_yz_auto_start_tick
            ? now - g_yz_auto_start_tick : 0;
        unsigned long long proof_delay = 780000ull;
        const char* delay_text = getenv("YZ_MOVEMENT_PROOF_DELAY_MS");
        if (delay_text && *delay_text) {
            const unsigned long long requested = _strtoui64(delay_text, NULL, 10);
            if (requested >= 60000ull)
                proof_delay = requested;
        }

        if (arm_file_mode < 0) {
            const char* requested = getenv("YZ_MOVEMENT_PROOF_ARM_FILE");
            const char* dialogue_requested = getenv(
                "YZ_MOVEMENT_PROOF_DIALOGUE_ARM_FILE");
            arm_file_mode = requested && *requested;
            dialogue_arm_file_mode = dialogue_requested &&
                *dialogue_requested;
            dialogue_arm_ready = !dialogue_arm_file_mode;
            if (arm_file_mode) {
                strncpy(arm_file, requested, sizeof(arm_file) - 1u);
                arm_file[sizeof(arm_file) - 1u] = '\0';
                fprintf(stderr,
                        "[movement-proof] visual arm required path=%s\n",
                        arm_file);
                fflush(stderr);
            }
            if (dialogue_arm_file_mode) {
                strncpy(dialogue_arm_file, dialogue_requested,
                        sizeof(dialogue_arm_file) - 1u);
                dialogue_arm_file[sizeof(dialogue_arm_file) - 1u] = '\0';
                fprintf(stderr,
                        "[movement-proof] initial Confirm waits for first "
                        "dialogue visual path=%s\n",
                        dialogue_arm_file);
                fflush(stderr);
            }
        }

        if (dialogue_arm_file_mode && !dialogue_arm_ready &&
            now >= next_dialogue_arm_poll) {
            const DWORD attrs = GetFileAttributesA(dialogue_arm_file);
            next_dialogue_arm_poll = now + 250ull;
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                dialogue_arm_ready = 1;
                fprintf(stderr,
                        "[movement-proof] first dialogue visual verified; "
                        "bounded Confirm enabled\n");
                fflush(stderr);
            }
        }

        if (!dialogue_timing_configured) {
            const char* period_text = getenv("YZ_DIALOGUE_PULSE_PERIOD_MS");
            const char* hold_text = getenv("YZ_DIALOGUE_PULSE_HOLD_MS");
            dialogue_period_ms = period_text && *period_text
                ? _strtoui64(period_text, NULL, 10) : 3000ull;
            dialogue_hold_ms = hold_text && *hold_text
                ? _strtoui64(hold_text, NULL, 10) : 1800ull;
            if (dialogue_period_ms < 1000ull)
                dialogue_period_ms = 1000ull;
            if (dialogue_hold_ms < 500ull)
                dialogue_hold_ms = 500ull;
            if (dialogue_hold_ms >= dialogue_period_ms)
                dialogue_hold_ms = dialogue_period_ms - 100ull;
            dialogue_timing_configured = 1;
        }
        if (skip_camera < 0)
            skip_camera = getenv("YZ_MOVEMENT_PROOF_SKIP_CAMERA") ? 1 : 0;
        if (!max_movement_legs_configured) {
            const char* max_legs_text =
                getenv("YZ_MOVEMENT_PROOF_MAX_LEGS");
            max_movement_legs = max_legs_text && *max_legs_text
                ? strtoul(max_legs_text, NULL, 10) : 0u;
            max_movement_legs_configured = 1;
        }
        if (!movement_hold_configured) {
            const char* hold_text = getenv("YZ_MOVEMENT_PROOF_HOLD_MS");
            const char* final_hold_text = getenv(
                "YZ_MOVEMENT_PROOF_FINAL_HOLD_MS");
            const char* lx_text = getenv("YZ_MOVEMENT_PROOF_LX");
            const char* ly_text = getenv("YZ_MOVEMENT_PROOF_LY");
            movement_hold_ms = hold_text && *hold_text
                ? _strtoui64(hold_text, NULL, 10) : 60000ull;
            if (movement_hold_ms < 1000ull)
                movement_hold_ms = 1000ull;
            final_movement_hold_ms = final_hold_text && *final_hold_text
                ? _strtoui64(final_hold_text, NULL, 10) : movement_hold_ms;
            if (final_movement_hold_ms < 1000ull)
                final_movement_hold_ms = 1000ull;
            movement_lx = (u8)(lx_text && *lx_text
                ? min(strtoul(lx_text, NULL, 10), 255ul) : 128ul);
            movement_ly = (u8)(ly_text && *ly_text
                ? min(strtoul(ly_text, NULL, 10), 255ul) : 0ul);
            movement_hold_configured = 1;
        }

        /* Advance authored dialogue with distinct rising edges while the
         * story continues to render.  Stop before the capture handshake so
         * no face button contaminates the movement samples. */
        if (g_yz_auto_start_tick && elapsed >= 180000ull &&
            InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0) == 0 &&
            dialogue_arm_ready &&
            (arm_file_mode || elapsed < proof_delay)) {
            /* The raw pad service can be polled much faster than the title's
             * high-level input cache.  At ~4 rendered FPS, the old 350 ms
             * down interval was repeatedly observed by cellPadGetData yet
             * cleared before the dialogue consumer sampled it.  Match the
             * proven menu-input cadence: hold across several guest frames,
             * then leave a full release interval for a distinct next edge. */
            const unsigned long long pulse =
                (elapsed - 180000ull) / dialogue_period_ms;
            const unsigned long long pulse_phase =
                (elapsed - 180000ull) % dialogue_period_ms;
            if (pulse_phase < dialogue_hold_ms) {
                hs->buttons |= CELL_PAD_CTRL_CROSS;
                hs->press_cross = 255;
                if (pulse != last_logged_pulse) {
                    last_logged_pulse = pulse;
                    fprintf(stderr,
                            "[movement-proof] accepted-dialogue pulse=%llu "
                            "elapsed_ms=%llu\n",
                            pulse, elapsed);
                    fflush(stderr);
                }
            }
        }

        if (g_yz_auto_start_tick && elapsed >= proof_delay) {
            if (!arm_file_mode) {
                InterlockedCompareExchange(
                    &g_yz_movement_proof_phase, 1, 0);
            } else if (now >= next_arm_file_poll) {
                const DWORD attrs = GetFileAttributesA(arm_file);
                next_arm_file_poll = now + 1000ull;
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
                    InterlockedCompareExchange(
                        &g_yz_movement_proof_phase, 1, 0) == 0) {
                    fprintf(stderr,
                            "[movement-proof] visually armed elapsed_ms=%llu "
                            "path=%s\n", elapsed, arm_file);
                    fflush(stderr);
                }
            }
        }

        /* A route leg may end in another authored encounter rather than open
         * gameplay.  Resume the same conservative Confirm cadence at phase 6,
         * while sparse renderer probes keep showing what it is advancing.
         * Once gameplay is visible again, arm the next forward leg by creating
         * arm-movement-2.txt, then -3.txt, and so on beside the first file. */
        if (arm_file_mode &&
            InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0) == 6) {
            const char* extension = strrchr(arm_file, '.');
            const unsigned next_leg = movement_leg + 1u;
            const int route_complete =
                max_movement_legs && movement_leg >= max_movement_legs;
            if (route_complete) {
                const LONG stable = InterlockedCompareExchange(
                    &g_yz_movement_stable_gameplay, 0, 0);
                const LONG dialogue_seen = InterlockedCompareExchange(
                    &g_yz_movement_post_dialogue_seen, 0, 0);
                const LONG gameplay_returned = InterlockedCompareExchange(
                    &g_yz_movement_gameplay_returned, 0, 0);
                if (stable && !route_complete_logged) {
                    fprintf(stderr,
                            "[movement-proof] route complete legs=%u; "
                            "stable gameplay confirmed; synthetic input "
                            "stopped\n",
                            movement_leg);
                    fflush(stderr);
                    route_complete_logged = 1;
                } else if (!stable) {
                    if (!route_wait_logged) {
                        fprintf(stderr,
                                "[movement-proof] movement leg complete; "
                                "waiting for post-movement stable gameplay\n");
                        fflush(stderr);
                        route_wait_logged = 1;
                    }
                    if (dialogue_seen && !gameplay_returned) {
                        if (!post_dialogue_tick) {
                            post_dialogue_tick = now;
                            last_post_logged_pulse = ~0ull;
                            fprintf(stderr,
                                    "[movement-proof] post-movement authored "
                                    "dialogue detected; Confirm pulses resumed\n");
                            fflush(stderr);
                        }
                        const unsigned long long post_elapsed =
                            now - post_dialogue_tick;
                        const unsigned long long pulse =
                            post_elapsed / dialogue_period_ms;
                        const unsigned long long pulse_phase =
                            post_elapsed % dialogue_period_ms;
                        if (pulse_phase < dialogue_hold_ms) {
                            hs->buttons |= CELL_PAD_CTRL_CROSS;
                            hs->press_cross = 255;
                            if (pulse != last_post_logged_pulse) {
                                last_post_logged_pulse = pulse;
                                fprintf(stderr,
                                        "[movement-proof] final-dialogue "
                                        "pulse=%llu elapsed_ms=%llu\n",
                                        pulse, elapsed);
                                fflush(stderr);
                            }
                        }
                    }
                }
            } else {
            if (!post_dialogue_tick) {
                post_dialogue_tick = now;
                last_post_logged_pulse = ~0ull;
                next_arm_path_logged = 0;
            }
            if (extension) {
                _snprintf_s(next_arm_file, sizeof(next_arm_file), _TRUNCATE,
                    "%.*s-%u%s", (int)(extension - arm_file), arm_file,
                    next_leg, extension);
            } else {
                _snprintf_s(next_arm_file, sizeof(next_arm_file), _TRUNCATE,
                    "%s-%u", arm_file, next_leg);
            }
            if (!next_arm_path_logged) {
                fprintf(stderr,
                        "[movement-proof] leg=%u complete; waiting for "
                        "visual dialogue/transition evidence; next arm "
                        "path=%s\n",
                        movement_leg, next_arm_file);
                fflush(stderr);
                next_arm_path_logged = 1;
            }

            if (now >= next_route_arm_file_poll) {
                const DWORD attrs = GetFileAttributesA(next_arm_file);
                next_route_arm_file_poll = now + 1000ull;
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
                    InterlockedCompareExchange(
                        &g_yz_movement_proof_phase, 1, 6) == 6) {
                    movement_leg = next_leg;
                    InterlockedExchange(
                        &g_yz_movement_proof_leg, (LONG)movement_leg);
                    InterlockedExchange(
                        &g_yz_movement_post_dialogue_seen, 0);
                    InterlockedExchange(
                        &g_yz_movement_gameplay_returned, 0);
                    InterlockedExchange(
                        &g_yz_movement_stable_gameplay, 0);
                    post_dialogue_tick = 0;
                    fprintf(stderr,
                            "[movement-proof] visually armed leg=%u "
                            "elapsed_ms=%llu path=%s\n",
                            movement_leg, elapsed, next_arm_file);
                    fflush(stderr);
                }
            }

            if (InterlockedCompareExchange(
                    &g_yz_movement_proof_phase, 0, 0) == 6) {
                const LONG gameplay_returned = InterlockedCompareExchange(
                    &g_yz_movement_gameplay_returned, 0, 0);
                const LONG dialogue_seen = InterlockedCompareExchange(
                    &g_yz_movement_post_dialogue_seen, 0, 0);
                const unsigned long long post_elapsed =
                    now - post_dialogue_tick;
                const unsigned long long pulse =
                    post_elapsed / dialogue_period_ms;
                const unsigned long long pulse_phase =
                    post_elapsed % dialogue_period_ms;
                if (dialogue_seen && !gameplay_returned &&
                    pulse_phase < dialogue_hold_ms) {
                    hs->buttons |= CELL_PAD_CTRL_CROSS;
                    hs->press_cross = 255;
                    if (pulse != last_post_logged_pulse) {
                        last_post_logged_pulse = pulse;
                        fprintf(stderr,
                                "[movement-proof] post-leg dialogue pulse=%llu "
                                "leg=%u elapsed_ms=%llu\n",
                                pulse, movement_leg, elapsed);
                        fflush(stderr);
                    }
                }
            }
            }
        }

        {
            LONG phase = InterlockedCompareExchange(
                &g_yz_movement_proof_phase, 0, 0);
            if (phase == 2) {
                if (skip_camera) {
                    yz_movement_frontier_snapshot("camera-skipped");
                    InterlockedCompareExchange(
                        &g_yz_movement_proof_phase, 3, 2);
                    fprintf(stderr,
                            "[movement-proof] right-stick camera skipped\n");
                    fflush(stderr);
                } else if (!movement_tick) {
                    movement_tick = now;
                    yz_movement_frontier_snapshot("before-camera");
                    fprintf(stderr,
                            "[movement-proof] right-stick camera begin "
                            "elapsed_ms=%llu\n", elapsed);
                    fflush(stderr);
                }
                if (!skip_camera)
                    hs->analog_rx = 255;
                if (!skip_camera && now - movement_tick >= 5000ull) {
                    yz_movement_frontier_snapshot("camera-held");
                    movement_tick = 0;
                    InterlockedCompareExchange(
                        &g_yz_movement_proof_phase, 3, 2);
                    fprintf(stderr,
                            "[movement-proof] right-stick camera complete "
                            "duration_ms=5000\n");
                    fflush(stderr);
                }
            } else if (phase == 4) {
                const unsigned long long active_hold_ms =
                    max_movement_legs && movement_leg >= max_movement_legs
                        ? final_movement_hold_ms : movement_hold_ms;
                if (!movement_tick) {
                    movement_tick = now;
                    yz_movement_frontier_snapshot("before-movement");
                    fprintf(stderr,
                            "[movement-proof] left-stick movement begin "
                            "leg=%u lx=%u ly=%u\n",
                            movement_leg, (unsigned)movement_lx,
                            (unsigned)movement_ly);
                    fflush(stderr);
                }
                hs->analog_lx = movement_lx;
                hs->analog_ly = movement_ly;
                if (now - movement_tick >= active_hold_ms) {
                    yz_movement_frontier_snapshot("movement-held");
                    movement_tick = 0;
                    InterlockedCompareExchange(
                        &g_yz_movement_proof_phase, 5, 4);
                    fprintf(stderr,
                            "[movement-proof] left-stick movement complete "
                            "leg=%u duration_ms=%llu\n", movement_leg,
                            active_hold_ms);
                    fflush(stderr);
                }
            }
        }
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

static void pad_poll_backend(void)
{
#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
    pad_merge_keyboard();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    if (max_connect == 0 || max_connect > CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();
#if PAD_BACKEND_XINPUT
    if (pad_keyboard_enabled()) {
        fprintf(stderr,
                "[cellPad] keyboard pad enabled: Enter=Start arrows=D-pad WASD=left-stick "
                "X/K=Cross C/L=Circle Z/J=Square V/I=Triangle\n");
        fflush(stderr);
    }
#endif
    if (pad_trace_enabled()) {
        fprintf(stderr, "[pad-trace] init connected_mask=0x%X\n", pad_connected_mask());
        fflush(stderr);
    }

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* Movie playback runs on the window thread while the guest pad reader keeps
 * running independently. Poll the host APIs directly and edge-detect Start so
 * the press that entered the movie cannot immediately skip it while held. */
void cellPad_host_movie_skip_begin(void)
{
#if PAD_BACKEND_XINPUT
    InterlockedExchange(&s_movie_skip_guest_seen, 0);
    InterlockedExchange(&s_movie_skip_poll_serial, 0);
    InterlockedExchange(&s_movie_skip_seen_poll, 0);
    InterlockedExchange(&s_movie_skip_active, 1);
#else
    s_movie_skip_guest_seen = 0;
    s_movie_skip_poll_serial = 0;
    s_movie_skip_seen_poll = 0;
    s_movie_skip_active = 1;
#endif
#if PAD_BACKEND_XINPUT
    s_movie_skip_down = pad_host_start_down();
#elif PAD_BACKEND_SDL2
    SDL_GameControllerUpdate();
    s_movie_skip_down = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; ++i) {
        SDL_GameController* gc = s_sdl_controllers[i];
        if (gc && SDL_GameControllerGetAttached(gc) &&
            SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))
            s_movie_skip_down = 1;
    }
#endif
}

int cellPad_host_movie_skip_guest_seen(void)
{
#if PAD_BACKEND_XINPUT
    const LONG seen = InterlockedCompareExchange(&s_movie_skip_guest_seen, 0, 0);
    const LONG poll = InterlockedCompareExchange(&s_movie_skip_poll_serial, 0, 0);
    const LONG seen_poll = InterlockedCompareExchange(&s_movie_skip_seen_poll, 0, 0);
    return seen && poll > seen_poll;
#else
    return s_movie_skip_guest_seen &&
           s_movie_skip_poll_serial > s_movie_skip_seen_poll;
#endif
}

void cellPad_host_movie_skip_end(void)
{
#if PAD_BACKEND_XINPUT
    InterlockedExchange(&s_movie_skip_active, 0);
#else
    s_movie_skip_active = 0;
#endif
}

int cellPad_host_movie_skip_requested(void)
{
    int down = 0;
#if PAD_BACKEND_XINPUT
    down = pad_host_start_down();
#elif PAD_BACKEND_SDL2
    SDL_GameControllerUpdate();
    for (int i = 0; i < PAD_MAX_HOST_PORTS; ++i) {
        SDL_GameController* gc = s_sdl_controllers[i];
        if (gc && SDL_GameControllerGetAttached(gc) &&
            SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))
            down = 1;
    }
#endif
    {
        const int pressed = down && !s_movie_skip_down;
        s_movie_skip_down = down;
        return pressed;
    }
}

s32 cellPadGetData(u32 port_no, CellPadData* data)
{
    static u16 last_buttons[PAD_MAX_HOST_PORTS];
    static u32 last_analog[PAD_MAX_HOST_PORTS];
    static u8 analog_seen[PAD_MAX_HOST_PORTS];
    static int first_call = 1;
    static u16 accepted_last_buttons[PAD_MAX_HOST_PORTS];
    static u32 accepted_last_analog[PAD_MAX_HOST_PORTS];
    static u8 accepted_analog_seen[PAD_MAX_HOST_PORTS];
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    if (pad_trace_enabled() && first_call) {
        first_call = 0;
        fprintf(stderr, "[pad-trace] first GetData port=%u connected_mask=0x%X\n",
                port_no, pad_connected_mask());
        fflush(stderr);
    }

    if (port_no >= PAD_MAX_HOST_PORTS || !s_host_state[port_no].connected) {
        data->len = 0;
        return CELL_OK;
    }

    PadHostState* hs = &s_host_state[port_no];
    /* A guest-visible input edge is the logical-stall detector's evidence
     * that cellPadGetData accepted it.  No per-frame increment: only actual
     * button or analog transitions count. */
    {
        const u32 accepted_analog = (u32)hs->analog_lx |
            ((u32)hs->analog_ly << 8) |
            ((u32)hs->analog_rx << 16) |
            ((u32)hs->analog_ry << 24);
        const int input_changed =
            hs->buttons != accepted_last_buttons[port_no] ||
            (accepted_analog_seen[port_no] &&
             accepted_analog != accepted_last_analog[port_no]);
        accepted_last_buttons[port_no] = hs->buttons;
        accepted_last_analog[port_no] = accepted_analog;
        accepted_analog_seen[port_no] = 1;
        if (input_changed) {
#ifdef _WIN32
            InterlockedIncrement(&s_accepted_input_serial);
#else
            __atomic_add_fetch(&s_accepted_input_serial, 1, __ATOMIC_RELAXED);
#endif
        }
    }
    u32 setting = s_port_setting[port_no];
#if PAD_BACKEND_XINPUT
    LONG movie_poll = 0;
    if (port_no == 0 && InterlockedCompareExchange(&s_movie_skip_active, 0, 0))
        movie_poll = InterlockedIncrement(&s_movie_skip_poll_serial);
    if (port_no == 0 && (hs->buttons & CELL_PAD_CTRL_START) &&
        InterlockedCompareExchange(&s_movie_skip_active, 0, 0) &&
        !InterlockedExchange(&s_movie_skip_guest_seen, 1)) {
        InterlockedExchange(&s_movie_skip_seen_poll, movie_poll);
#else
    int movie_poll = 0;
    if (port_no == 0 && s_movie_skip_active)
        movie_poll = ++s_movie_skip_poll_serial;
    if (port_no == 0 && (hs->buttons & CELL_PAD_CTRL_START) &&
        s_movie_skip_active && !s_movie_skip_guest_seen) {
        s_movie_skip_guest_seen = 1;
        s_movie_skip_seen_poll = movie_poll;
#endif
        fprintf(stderr, "[cellPad] guest observed movie Start request\n");
        fflush(stderr);
    }
    if (pad_trace_enabled() && hs->buttons != last_buttons[port_no]) {
        fprintf(stderr, "[pad-trace] port=%u buttons=0x%04X\n", port_no, hs->buttons);
        fflush(stderr);
        last_buttons[port_no] = hs->buttons;
    }
    if (pad_trace_enabled()) {
        const u32 analog = (u32)hs->analog_lx |
            ((u32)hs->analog_ly << 8) |
            ((u32)hs->analog_rx << 16) |
            ((u32)hs->analog_ry << 24);
        if (!analog_seen[port_no] || analog != last_analog[port_no]) {
            fprintf(stderr,
                    "[pad-trace] port=%u analog lx=%u ly=%u rx=%u ry=%u\n",
                    port_no, hs->analog_lx, hs->analog_ly,
                    hs->analog_rx, hs->analog_ry);
            fflush(stderr);
            last_analog[port_no] = analog;
            analog_seen[port_no] = 1;
        }
    }

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    data->button[0] = (u16)len;
    data->button[1] = 0; /* reserved */

    /* Digital buttons: hs->buttons packs DIGITAL1 (SELECT..LEFT, bits 0-7) and
     * DIGITAL2 (L2..SQUARE, bits 8-15) into one 16-bit mask; the guest reads them
     * as two separate words. Writing the whole mask into DIGITAL1 with DIGITAL2=0
     * left every face button and both shoulder pairs dead. */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* Analog sticks */
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }

    /* CellPadData is guest big-endian: byte-swap len + every button (else the game
     * reads byte-swapped buttons/analog). */
    data->len = (s32)ps3_bswap32((u32)data->len);
    for (size_t i = 0; i < sizeof(data->button)/sizeof(data->button[0]); i++)
        data->button[i] = ps3_bswap16(data->button[i]);
    return CELL_OK;
}

u32 yz_pad_guest_input_serial(void)
{
#ifdef _WIN32
    return (u32)InterlockedCompareExchange(
        &s_accepted_input_serial, 0, 0);
#else
    return (u32)__atomic_load_n(
        &s_accepted_input_serial, __ATOMIC_RELAXED);
#endif
}

s32 cellPadGetInfo2(CellPadInfo2* info)
{
    static u32 last_mask = ~0u;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();
    if (pad_trace_enabled()) {
        u32 mask = pad_connected_mask();
        if (mask != last_mask) {
            fprintf(stderr, "[pad-trace] GetInfo2 connected_mask=0x%X\n", mask);
            fflush(stderr);
            last_mask = mask;
        }
    }

    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        if (s_host_state[i].connected) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* CellPadInfo2 is guest big-endian: byte-swap all u32 fields. */
    info->max_connect = ps3_bswap32(info->max_connect);
    info->now_connect = ps3_bswap32(info->now_connect);
    for (size_t i = 0; i < sizeof(info->port_status)/sizeof(info->port_status[0]); i++) {
        info->port_status[i]       = ps3_bswap32(info->port_status[i]);
        info->port_setting[i]      = ps3_bswap32(info->port_setting[i]);
        info->device_capability[i] = ps3_bswap32(info->device_capability[i]);
        info->device_type[i]       = ps3_bswap32(info->device_type[i]);
    }

    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    memset(info, 0, sizeof(CellPadCapabilityInfo));

    /* Report standard DualShock 3 capabilities */
    info->info[0] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                   | CELL_PAD_CAPABILITY_PRESS_MODE
                   | CELL_PAD_CAPABILITY_SENSOR_MODE
                   | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                   | CELL_PAD_CAPABILITY_ACTUATOR;

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257),
            100 /* duration ms */
        );
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
