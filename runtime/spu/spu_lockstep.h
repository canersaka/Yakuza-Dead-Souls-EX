/*
 * ps3recomp - YZ_SPU_LOCKSTEP: SPU round-robin run token (s42 diagnostic)
 *
 * Interventional experiment for the M8 (ORDERING) finalist on the s41 release-
 * engine wall (ledger #101): serialize all lifted-SPU host threads behind one
 * global run token so only one SPU executes lifted code at a time, in
 * registration order, quantum-based round robin. Design + adversarial review:
 * scratch/s42_lockstep_design.md (v2, POST-REVIEW -- the (star)REVIEW DELTAS
 * section is binding) and scratch/s42_lockstep_review.md. Env-gated, default
 * OFF (YZ_SPU_LOCKSTEP unset). Diagnostic/interventional only -- never a
 * shipping behavior change (LESSONS #13).
 *
 * ★REVIEW DELTAS implemented here (see spu_lockstep.c for the mechanism):
 *  (B) Gate site is SPU_DRAIN (spu_context.h), not spu_indirect_branch -- the
 *      lifter's per-instruction trampoline re-entry is the real hot loop.
 *  (C) The decrementer is frozen to wall-time-while-holding-the-token: every
 *      token acquire advances ctx->dec_start_tb by exactly the wall-clock
 *      span since this ctx last released the token (or was registered, if it
 *      never held it before), so RdDec's existing
 *      `ppu_timebase_now() - dec_start_tb` formula automatically excludes
 *      time spent waiting for a turn -- zero changes needed to the RdDec/
 *      WrDec code in spu_channels.c, and the non-lockstep path is untouched
 *      (this module writes dec_start_tb ONLY when armed).
 *  (D) Both GETLLAR legs (spu_dma.h fast cached path + the locked slow path)
 *      call yz_lockstep_tick.
 *  (E) Token-pass happens before any spu_idle_yield Sleep (the getllar
 *      backoff ladder is downstream of the tick call, so a passed-off SPU
 *      never sleeps while holding the token).
 *
 * Threading model: each spu_context is pinned to exactly one host thread for
 * its lifetime (SPU_THREAD_LOCAL trampoline design); this module gates
 * (blocks non-holders) rather than migrating, so no per-thread state moves.
 */
#ifndef SPU_LOCKSTEP_H
#define SPU_LOCKSTEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;   /* full type in spu_context.h; only pointers needed here */

/* -1 unarmed (never checked env), 0 off, 1 on. Mirrors g_yz_fltrec_on's
 * arm-once pattern (spu_fltrec.h) so every call site is a single predicted-
 * not-taken branch when the flag is unset. */
extern volatile int g_yz_lockstep_on;

/* Arms from env (YZ_SPU_LOCKSTEP, YZ_LOCKSTEP_QUANTUM), idempotent,
 * thread-safe (atomic_flag-guarded like yz_fltrec_enabled). Prints the
 * "[lockstep] ARMED quantum=N" banner exactly once, the first time this is
 * reached -- which is always inside the first yz_lockstep_register() call,
 * since nothing executes lifted SPU code before its own thread registers. */
int yz_lockstep_enabled(void);

/* Register/unregister this ctx's host thread in the round-robin ring.
 * yz_lockstep_register BLOCKS until this ctx actually holds the token (the
 * ring's first-ever member is granted it immediately; later members wait
 * their turn) -- callers must not run any lifted SPU code before it returns.
 * Call register() at spu_exec_thread_proc entry, unregister() in its
 * outermost frame (real thread exit only -- the context-replacement/restart
 * longjmp unwinds re-enter on the SAME host thread and must NOT unregister;
 * see lv2_register.c). Both are no-ops when YZ_SPU_LOCKSTEP is unset. */
void yz_lockstep_register(struct spu_context* ctx);
void yz_lockstep_unregister(struct spu_context* ctx);

/* Cheap per-site call: counts one quantum unit (a SPU_DRAIN re-entry or a
 * GETLLAR) for `ctx` and hands the token to the next runnable ring member
 * once the quantum (YZ_LOCKSTEP_QUANTUM, default 65536) expires, blocking
 * the caller until it is granted the token again. A real out-of-line
 * function (matching the existing spu_task_launch_check/spu_prof_hop
 * SPU_DRAIN-site convention) rather than an inline, because spu_context's
 * full definition is not yet visible when this header is included from
 * spu_context.h itself. First line is a single `g_yz_lockstep_on == 0`
 * check when unarmed. */
void yz_lockstep_tick(struct spu_context* ctx);

/* Bracket a blocking channel wait (spu_ch_wait, spu_channels.c): release the
 * token before the OS-level wait (so a waker that itself needs the token to
 * proceed is never blocked on this ctx), and rejoin the rotation on wake.
 * No-ops when unset. */
void yz_lockstep_block_begin(struct spu_context* ctx);
void yz_lockstep_block_end(struct spu_context* ctx);

#ifdef __cplusplus
}
#endif
#endif /* SPU_LOCKSTEP_H */
