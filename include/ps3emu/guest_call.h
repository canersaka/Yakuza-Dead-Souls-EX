/*
 * ps3recomp - Guest callback dispatch hook
 *
 * The HLE runtime is plain C compiled to host. When a system service
 * (cellSysutil events, cellSaveData callbacks, etc.) needs to invoke a
 * guest-side callback the game registered, the runtime can't call it
 * directly: the callback's "function pointer" is actually a guest VM
 * address pointing at an OPD (function descriptor holding the entry and
 * TOC words).
 *
 * The recompiled code base owns the dispatcher (it has the giant
 * function table mapping guest_addr -> host_func and the trampoline
 * runner). It registers a `ps3_guest_caller_fn` that the HLE runtime
 * uses whenever it needs to fire a guest callback. This keeps the
 * runtime independent of any specific game's recompiled code.
 *
 * Calling convention for the hook: the guest function is invoked with
 * up to eight 64-bit args mapped to PPC r3..r10. The hook is responsible
 * for setting up a clean ppu_context, plumbing the args, dispatching
 * via OPD, and draining trampolines before returning.
 */

#ifndef PS3EMU_GUEST_CALL_H
#define PS3EMU_GUEST_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hook signature.
 *   opd_addr  - guest address of the OPD to invoke (entry and TOC words)
 *   arg0..7   - first eight args, placed in r3..r10 by the caller
 *
 * The hook runs the callback synchronously; cellSysutilCheckCallback relies
 * on that so games observe the callback complete before it returns. */
typedef void (*ps3_guest_caller_fn)(uint32_t opd_addr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5,
                                    uint64_t arg6, uint64_t arg7);

/* Returning variant: identical dispatch, but hands back the guest function's
 * r3 return value. Needed by the few callbacks whose return value carries
 * protocol meaning -- e.g. SceNpTrophyStatusCallback, where a negative
 * return instructs sceNpTrophyRegisterContext to abort registration.
 * The void variant serves the common fire-and-forget case. */
typedef uint64_t (*ps3_guest_caller_ret_fn)(uint32_t opd_addr,
                                            uint64_t arg0, uint64_t arg1,
                                            uint64_t arg2, uint64_t arg3,
                                            uint64_t arg4, uint64_t arg5,
                                            uint64_t arg6, uint64_t arg7);

/* Installed by the runner at startup (yakuza/main.cpp). NULL until
 * installed; HLE bridges that need to call back into guest code must
 * check for NULL before invoking. */
extern ps3_guest_caller_fn g_ps3_guest_caller;

/* NULL until installed (both are defined in libs/system/cellSysutil.c and
 * installed together by the game's host code). HLE code that needs a return
 * value should prefer this hook and fall back to the void hook -- treating
 * the callback as having returned 0 -- when only the void hook is set. */
extern ps3_guest_caller_ret_fn g_ps3_guest_caller_ret;

/* Convenience wrapper: checks for NULL and invokes if set. */
static inline void ps3_invoke_guest(uint32_t opd_addr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5,
                                    uint64_t arg6, uint64_t arg7)
{
    if (g_ps3_guest_caller)
        g_ps3_guest_caller(opd_addr, arg0, arg1, arg2, arg3,
                           arg4, arg5, arg6, arg7);
}

#ifdef __cplusplus
}
#endif

#endif
