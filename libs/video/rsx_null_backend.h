/*
 * ps3recomp - Null RSX Backend (Win32 window + color clear)
 *
 * Owns the Win32 window, counts flips, forwards keyboard events to cellPad
 * and updates the title statistics; rendering is done by rsx_live_draw. When
 * no GPU renderer owns the window it paints the RSX clear color with GDI.
 */

#ifndef PS3RECOMP_RSX_NULL_BACKEND_H
#define PS3RECOMP_RSX_NULL_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create and register the null backend.
 * Opens a window of the specified size and starts accepting RSX commands.
 * Returns 0 on success. */
int rsx_null_backend_init(u32 width, u32 height, const char* title);

/* Shut down the null backend and close the window. */
void rsx_null_backend_shutdown(void);

/* Process Win32 messages. Call this from the game's main loop
 * or from a timer/idle callback. Returns 0 normally, -1 if the
 * window was closed. */
int rsx_null_backend_pump_messages(void);

/* The window handle (HWND cast to void*), so a GPU backend can bind a swap
 * chain to the existing present window. NULL before init. */
void* rsx_null_backend_get_hwnd(void);

/* Suppress the GDI clear-color present + WM_PAINT fill. A GPU backend (live
 * draw) that owns a flip-model swap chain on this HWND calls this with 1
 * so the two don't fight over the window surface. Default: not suppressed. */
void rsx_null_backend_suppress_present(int on);
/* True only while this registered backend is a window/flip counter, with its
 * GDI output disabled because a live GPU renderer owns the window. */
int rsx_null_backend_graphics_suppressed(void);

#ifdef __cplusplus
}
#endif
#endif
