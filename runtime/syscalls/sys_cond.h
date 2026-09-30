/*
 * ps3recomp - Condition variable syscalls
 */

#ifndef SYS_COND_H
#define SYS_COND_H

#include "lv2_syscall_table.h"
#include "sys_mutex.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_COND_MAX  256

/* Per-cond committed-waiter thread-id registry.
 * sys_cond_signal_to must distinguish "target thread is parked on this cond"
 * (deliver) from "target is not waiting" (EPERM). The
 * committed/pending counters can't answer per-thread membership, so waits
 * record their guest tid here (under the sig lock) for the wait's lifetime.
 * Up to 32 concurrent waiters per cond are tracked; overflow entries are
 * untracked (signal_to would then miss them). */
#define SYS_COND_WAITER_TIDS_MAX  32

typedef struct sys_cond_info {
    int      active;
    uint32_t mutex_id;   /* associated mutex */

    /* Rendezvous state: real lv2 enqueues the waiter on the kernel sleep
     * queue at wait-syscall entry (under the kernel lock), so a signal
     * arriving any time after that is delivered even if the waiter hasn't
     * parked in the host CV yet. A bare Win32 CV drops that signal
     * (edge-triggered). `committed`/`pending` under the internal sig lock
     * close the window without touching the guest mutex (so the wait never
     * holds one lock while waiting on another). Signals with no committed
     * waiter are still discarded, as on lv2. */
    int      committed;  /* waiters inside the wait syscall */
    int      pending;    /* signals held for committed waiters */

    /* Guest tids of committed waiters (0 = free slot); guarded by the sig
     * lock. See SYS_COND_WAITER_TIDS_MAX above. */
    uint64_t waiter_tids[SYS_COND_WAITER_TIDS_MAX];

#ifdef _WIN32
    CONDITION_VARIABLE cv;
    CRITICAL_SECTION   sig_cs;
    int                sig_cs_init;
#else
    pthread_cond_t     cv;
    pthread_mutex_t    sig_mtx;
    int                sig_mtx_init;
#endif

} sys_cond_info;

extern sys_cond_info g_sys_conds[SYS_COND_MAX];

/* Syscall handlers */
int64_t sys_cond_create(ppu_context* ctx);
int64_t sys_cond_destroy(ppu_context* ctx);
int64_t sys_cond_wait(ppu_context* ctx);
int64_t sys_cond_signal(ppu_context* ctx);
int64_t sys_cond_signal_all(ppu_context* ctx);
int64_t sys_cond_signal_to(ppu_context* ctx);

/* Registration */
void sys_cond_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_COND_H */
