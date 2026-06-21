/* spu_lifted_fallback.c — lv2 glue: run a LIFTED SPU job as a PPU-fallback.
 *
 * The lv2 SPU-thread-group layer (lv2_register.c) runs each SPU thread by looking
 * up a PPU-fallback for its image entry point and calling it on a host thread with
 * (tid, args_ea, args_size, user). Register this wrapper with `user` = the lifted
 * entry fn (from spu_lifter), and lv2 will execute the lifted SPU code on the
 * thread's local store with the SPURS task arg in r3:
 *
 *     spu_register_ppu_fallback(image_entry, spu_lifted_fallback, (void*)lifted_fn);
 *
 * This bridges the lv2 SPU-thread layer to the lifted-execution layer — the brick
 * the flOw SPU integration needs (feed lifted jobs as task bodies).
 */
#include "../spu/spu_lifted_job.h"
#include <stdint.h>

/* defined in lv2_register.c — the SPU thread's 256 KB local store */
extern uint8_t* spu_thread_get_local_store(uint32_t tid);

int32_t spu_lifted_fallback(uint32_t tid, uint32_t args_ea,
                            uint32_t args_size, void* user)
{
    (void)args_size;
    return spu_run_lifted_job((spu_lifted_entry_fn)user,
                              spu_thread_get_local_store(tid), args_ea);
}
