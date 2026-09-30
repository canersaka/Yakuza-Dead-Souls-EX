/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend: XInput plus a keyboard-backed virtual pad.
 */

#include "cellPad.h"
#include "ps3emu/endian.h"   /* ps3_bswap16/32: CellPadData/Info2 are guest big-endian */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports at most 4 */

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
static volatile LONG s_window_key_state[256];
static int           s_movie_skip_down = 0;
static volatile LONG s_movie_skip_active = 0;
static volatile LONG s_movie_skip_guest_seen = 0;
static volatile LONG s_movie_skip_poll_serial = 0;
static volatile LONG s_movie_skip_seen_poll = 0;



/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

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
         * (256 - x), not 255 - x: the latter turns a centered stick into 127,
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


static int pad_key_down(int virtual_key, int allow_async)
{
    const int message_down =
        virtual_key >= 0 && virtual_key < 256 &&
        InterlockedCompareExchange(&s_window_key_state[virtual_key], 0, 0) != 0;
    return message_down || (allow_async && (GetAsyncKeyState(virtual_key) & 0x8000) != 0);
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
    if ((pad_keyboard_window_focused() && pad_key_down(VK_RETURN, 1)) ||
        pad_window_key_down(VK_RETURN))
        down = 1;
    return down;
}

/* Merge a keyboard-backed virtual controller into port 0.  This deliberately
 * runs after XInput so a real controller and the keyboard can be used at the
 * same time.  The keyboard also keeps port 0 connected when no XInput device
 * is present, which lets title/menu input work without a controller. */
static void pad_merge_keyboard(void)
{
    const int keyboard_focused = pad_keyboard_window_focused();
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
     * foreground so background key state cannot reach guest input. Each key
     * read below receives the independently sampled focus flag. */
    if (!keyboard_focused && !pad_window_key_any_down())
        return;

    u16 btns = hs->buttons;
    if (pad_key_down(VK_BACK, keyboard_focused))   btns |= CELL_PAD_CTRL_SELECT;
    if (pad_key_down(VK_RETURN, keyboard_focused)) btns |= CELL_PAD_CTRL_START;
    if (pad_key_down(VK_UP, keyboard_focused))     btns |= CELL_PAD_CTRL_UP;
    if (pad_key_down(VK_RIGHT, keyboard_focused))  btns |= CELL_PAD_CTRL_RIGHT;
    if (pad_key_down(VK_DOWN, keyboard_focused))   btns |= CELL_PAD_CTRL_DOWN;
    if (pad_key_down(VK_LEFT, keyboard_focused))   btns |= CELL_PAD_CTRL_LEFT;
    if (pad_key_down('1', keyboard_focused))       btns |= CELL_PAD_CTRL_L2;
    if (pad_key_down('3', keyboard_focused))       btns |= CELL_PAD_CTRL_R2;
    if (pad_key_down('Q', keyboard_focused))       btns |= CELL_PAD_CTRL_L1;
    if (pad_key_down('E', keyboard_focused))       btns |= CELL_PAD_CTRL_R1;
    if (pad_key_down('V', keyboard_focused) || pad_key_down('I', keyboard_focused)) btns |= CELL_PAD_CTRL_TRIANGLE;
    if (pad_key_down('C', keyboard_focused) || pad_key_down('L', keyboard_focused)) btns |= CELL_PAD_CTRL_CIRCLE;
    if (pad_key_down('X', keyboard_focused) || pad_key_down('K', keyboard_focused)) btns |= CELL_PAD_CTRL_CROSS;
    if (pad_key_down('Z', keyboard_focused) || pad_key_down('J', keyboard_focused)) btns |= CELL_PAD_CTRL_SQUARE;
    hs->buttons = btns;

    /* WASD supplies a full-strength left stick while arrows remain a D-pad. */
    if (pad_key_down('A', keyboard_focused)) hs->analog_lx = 0;
    if (pad_key_down('D', keyboard_focused)) hs->analog_lx = 255;
    if (pad_key_down('W', keyboard_focused)) hs->analog_ly = 0;
    if (pad_key_down('S', keyboard_focused)) hs->analog_ly = 255;
    /* Numeric keypad supplies camera/right-stick input through the same
     * focus-checked physical or process-local window-key path. */
    if (pad_key_down(VK_NUMPAD4, keyboard_focused)) hs->analog_rx = 0;
    if (pad_key_down(VK_NUMPAD6, keyboard_focused)) hs->analog_rx = 255;
    if (pad_key_down(VK_NUMPAD8, keyboard_focused)) hs->analog_ry = 0;
    if (pad_key_down(VK_NUMPAD2, keyboard_focused)) hs->analog_ry = 255;

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

}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

static void pad_poll_backend(void)
{
    pad_poll_xinput();
    pad_merge_keyboard();
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
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
    fprintf(stderr,
            "[cellPad] keyboard pad enabled: Enter=Start arrows=D-pad WASD=left-stick "
            "X/K=Cross C/L=Circle Z/J=Square V/I=Triangle\n");
    fflush(stderr);

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

/* Movie playback runs on the window thread while the guest pad reader keeps
 * running independently. Poll the host APIs directly and edge-detect Start so
 * the press that entered the movie cannot immediately skip it while held. */
void cellPad_host_movie_skip_begin(void)
{
    InterlockedExchange(&s_movie_skip_guest_seen, 0);
    InterlockedExchange(&s_movie_skip_poll_serial, 0);
    InterlockedExchange(&s_movie_skip_seen_poll, 0);
    InterlockedExchange(&s_movie_skip_active, 1);
    s_movie_skip_down = pad_host_start_down();
}

int cellPad_host_movie_skip_guest_seen(void)
{
    const LONG seen = InterlockedCompareExchange(&s_movie_skip_guest_seen, 0, 0);
    const LONG poll = InterlockedCompareExchange(&s_movie_skip_poll_serial, 0, 0);
    const LONG seen_poll = InterlockedCompareExchange(&s_movie_skip_seen_poll, 0, 0);
    return seen && poll > seen_poll;
}

void cellPad_host_movie_skip_end(void)
{
    InterlockedExchange(&s_movie_skip_active, 0);
}

int cellPad_host_movie_skip_requested(void)
{
    const int down = pad_host_start_down();
    {
        const int pressed = down && !s_movie_skip_down;
        s_movie_skip_down = down;
        return pressed;
    }
}

s32 cellPadGetData(u32 port_no, CellPadData* data)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();


    if (port_no >= PAD_MAX_HOST_PORTS || !s_host_state[port_no].connected) {
        data->len = 0;
        return CELL_OK;
    }

    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];
    LONG movie_poll = 0;
    if (port_no == 0 && InterlockedCompareExchange(&s_movie_skip_active, 0, 0))
        movie_poll = InterlockedIncrement(&s_movie_skip_poll_serial);
    if (port_no == 0 && (hs->buttons & CELL_PAD_CTRL_START) &&
        InterlockedCompareExchange(&s_movie_skip_active, 0, 0) &&
        !InterlockedExchange(&s_movie_skip_guest_seen, 1)) {
        InterlockedExchange(&s_movie_skip_seen_poll, movie_poll);
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
     * would leave every face button and both shoulder pairs dead. */
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


s32 cellPadGetInfo2(CellPadInfo2* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

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

    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation: state is polled fresh */
    return CELL_OK;
}
