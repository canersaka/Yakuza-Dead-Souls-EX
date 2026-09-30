/*
 * ps3recomp - live NV4097 draw path
 *
 * A full NV4097 -> D3D12 draw pipeline: the rsx_dispatch register-file model
 * feeds an NV40 VP/FP -> HLSL decompiler and a PSO/sampler/texture engine that
 * renders the game's geometry. The live FIFO consumer (yz_rsx_method in
 * yakuza/yz_rsx_methods.inc) drives it with the game's own method stream,
 * rendering into a D3D12 present surface.
 *
 * Data flow (live):
 *   game (t1) writes NV methods to the FIFO ring
 *     -> yz_rsx_method(method, arg)                         [FIFO consumer hook]
 *        -> rsx_live_draw_method(method, arg)               [this module]
 *           -> rsx_dispatch_method(...)  (register file + BEGIN/END/DRAW/FLIP)
 *              -> sink callbacks -> D3D12 PSO + draw into the flip surface
 *   flip method (0xE944) -> rsx_live_draw_present(buffer_id) -> Present()
 *
 * Clean-room: method numbers/semantics from envytools rnndb, Mesa nv30 and
 * psdevwiki. No GPL code.
 *
 * Windows/D3D12 only; a no-op stub compiles elsewhere.
 */

#ifndef PS3RECOMP_RSX_LIVE_DRAW_H
#define PS3RECOMP_RSX_LIVE_DRAW_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Caller owns serialization and publication. This entry consumes renderer
 * state only: external FIFO/guest side effects must be split out by the caller.
 * A refused group changes nothing and must follow the ordinary method path. */
struct rsx_state;
int rsx_live_draw_prepared_packet_supported(u32 header);
int rsx_live_draw_graphics_packet(u32 header,const u32* arguments,struct rsx_state* protocol);

/* Guest-memory resolver: given an RSX location (0 = RSX local VRAM,
 * 1 = main/IO memory) and a byte offset, return a host pointer to at least
 * `min_bytes` of readable guest memory, or NULL if the region is invalid.
 * The runtime supplies this (it owns the vm_base map + the gcm local carve).
 */
typedef const u8* (*rsx_live_guest_ptr_fn)(void* user, u32 location, u32 offset,
                                           u32 min_bytes);

int rsx_live_draw_set_early_geometry_cache(int enabled);

/* Whether the live draw path is available (always on Windows).
 * Cheap; safe to call from the hot method hook. */
int  rsx_live_draw_enabled(void);

/* Bring up the D3D12 device + swap chain bound to an existing HWND (the
 * runtime already opens the present window). Returns 0 on success. Safe to
 * call when disabled (returns 0 and stays inert). `hwnd` is an HWND cast to
 * void* to keep this header windows.h-free. */
int  rsx_live_draw_init(void* hwnd, u32 width, u32 height,
                        rsx_live_guest_ptr_fn guest_ptr, void* guest_user);

/* Seed the dispatcher's register file / transform program from a captured or
 * live initial context (optional; the live path can also just stream). */
void rsx_live_draw_seed_registers(const u32* regs, u32 count);
void rsx_live_draw_seed_transform_program(const u32* words, u32 count);
void rsx_live_draw_seed_transform_constants(const u32* words, u32 count);

/* Register a scanout buffer from sys_rsx_context_attribute(0x104). A flip's
 * argument indexes this table; it is not necessarily the current render
 * target when the frame is submitted. */
void rsx_live_draw_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                      u32 pitch, u32 width, u32 height);

/* Feed one NV method write. No-op when disabled or uninitialized. */
void rsx_live_draw_method(u32 method, u32 arg);

/* Block until the GPU finishes all queued draws (RSX SET_REFERENCE / sync
 * fence), so REF advances only after the GPU has really caught up. */
void rsx_live_draw_flush(void);

/* Present the given display buffer id (called from the flip path). */
void rsx_live_draw_present(u32 buffer_id);

/* Movie playback: while on, the guest method stream is ignored so a host-decoded
 * movie can own the window without the guest's draws racing g.list. */
void rsx_live_draw_set_movie_mode(int on);

/* Present a host-decoded RGBA8 frame (w*h*4, row pitch w*4) straight to the
 * swap-chain backbuffer. Call from one thread with movie mode on. */
void rsx_live_draw_present_rgba(const uint8_t* rgba, u32 w, u32 h);

/* Count of real game frames the live-draw engine has presented (the game's own
 * 0xE944 flips carrying accumulated draws) -- distinct from the null backend's
 * per-vblank flip count, which advances every vblank even when the game is not
 * producing frames. Cheap; safe to poll from the title path. */
u32  rsx_live_draw_get_frames(void);

/* Monotonic count of draw groups actually submitted by the live renderer.
 * Used by the optional FIFO wait aggregate; no synchronization or reset. */
u64  rsx_live_draw_get_completed_draws(void);

/* Draws in the last completed frame (title-bar telemetry). */
u32  rsx_live_draw_get_last_draws(void);

/* Informational rolling presented FPS for the window title. */
double rsx_live_draw_get_present_fps(void);

/* Release all D3D12 resources. */
void rsx_live_draw_shutdown(void);
int rsx_live_draw_set_geometry_cache(int enabled);
void rsx_live_draw_dump_geometry_cache(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_LIVE_DRAW_H */
