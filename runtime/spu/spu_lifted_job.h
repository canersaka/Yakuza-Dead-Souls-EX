/* spu_lifted_job.h — run a lifted SPU job with the SPURS task ABI.
 *
 * The bridge between the lv2 SPU-thread-group layer (runtime/syscalls/lv2_register.c,
 * which runs SPU threads as registered PPU-fallbacks) and the lifted-execution layer
 * (a lifted spu_func from spu_lifter). A SPURS task / SPU thread receives its argument
 * (the job/task descriptor effective address) in r3 and runs against its 256 KB local
 * store; this helper sets that up, runs the lifted entry, and bridges the local store.
 *
 * Wiring into lv2: register `spu_lifted_fallback` for an SPU image's entry point with
 * `user` = the lifted entry fn; lv2's spu_fallback_thread_proc then calls it with
 * (tid, args_ea, args_size, user), and we run the lifted job on that thread's LS.
 */
#ifndef SPU_LIFTED_JOB_H
#define SPU_LIFTED_JOB_H

#include "spu_context.h"
#include <string.h>

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* Run a lifted SPU job. `local_store` (256 KB, may be NULL) is the SPU thread's
 * local store; it is brought into the context, the job's arg EA is placed in r3
 * (the SPURS task ABI), the lifted entry runs, and the LS is written back.
 * The caller links the channel ABI (spu_wrch/spu_rdch/...) that the lifted code
 * uses to reach DMA / mailboxes / events. Returns the job's exit code (0). */
static inline int32_t spu_run_lifted_job(spu_lifted_entry_fn entry,
                                         uint8_t* local_store,
                                         uint32_t args_ea)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);  /* job's LS in */
    ctx.gpr[3]._u32[0] = args_ea;                               /* SPURS task arg -> r3 */
    entry(&ctx);                                                /* run the lifted job  */
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);  /* LS back out */
    return 0;
}

/* lv2 PPU-fallback wrapper: signature matches spu_ppu_fallback_fn so it can be
 * registered via spu_register_ppu_fallback(entry_point, spu_lifted_fallback, fn).
 * Defined where spu_thread_get_local_store() is available (the lv2 TU); declared
 * here for callers. (uint32_t tid, uint32_t args_ea, uint32_t args_size, void* user) */
int32_t spu_lifted_fallback(uint32_t tid, uint32_t args_ea,
                            uint32_t args_size, void* user);

#endif /* SPU_LIFTED_JOB_H */
