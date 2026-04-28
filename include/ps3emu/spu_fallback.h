/*
 * ps3emu/spu_fallback.h — PPU-side SPU job fallback registry
 *
 * ps3recomp doesn't execute SPU code (no JIT for SPU ISA, no SPU LS model).
 * For most games this is fine — SPU jobs that produce side-effects only the
 * PPU later checks for can be no-ops and the title still progresses.
 *
 * Some games REQUIRE SPU output: PhyreEngine asset decompressors, audio
 * mixers, particle simulations, etc. write back data the PPU then reads.
 * Stubbing those silently leaves the PPU reading garbage.
 *
 * This API lets a game (or a per-game shim) register a PPU-side handler
 * keyed on the SPU image entry point. When the SPU thread group is started,
 * each thread's entry is looked up; if a handler is registered, it runs as
 * a host thread and the group's join completion waits on it. If no handler
 * matches, behaviour is the existing "instantly succeed with status 0".
 *
 * Typical usage from a game's hle_modules.cpp at startup:
 *
 *     static void my_decompressor_fallback(uint32_t tid, uint32_t args_ea,
 *                                          uint32_t args_size, void* user)
 *     {
 *         // Read job descriptor from guest memory at args_ea, do the work
 *         // on the host, write results back via vm_write* helpers.
 *     }
 *     spu_register_ppu_fallback(0x00012340, my_decompressor_fallback, NULL);
 *
 * The entry_point key is the value at sys_spu_image+4 (the SPU ELF entry).
 * For PhyreEngine SPU jobs, that's the .text start of the job binary.
 *
 * Thread-safety: registration is not thread-safe; call all
 * spu_register_ppu_fallback() at startup before any sys_spu_thread_group_*
 * activity.
 */

#ifndef PS3EMU_SPU_FALLBACK_H
#define PS3EMU_SPU_FALLBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Handler signature for an SPU job's PPU equivalent.
 *   tid       — the synthesized SPU thread id assigned by sys_spu_thread_initialize
 *   args_ea   — guest EA of the args block passed via sys_spu_thread_set_argument
 *   args_size — size in bytes of the args block (or 0 if unknown)
 *   user      — opaque pointer supplied at registration time
 *
 * The handler runs on a host thread spawned by sys_spu_thread_group_start.
 * It should write any side-effects to guest memory via vm_write*() helpers.
 * Return value goes into the SPU thread's exit status (read by
 * sys_spu_thread_get_exit_status / reported by sys_spu_thread_group_join).
 */
typedef int32_t (*spu_ppu_fallback_fn)(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user);

/*
 * Register a PPU fallback for an SPU job identified by its image entry point.
 * Multiple registrations for the same entry replace the previous handler.
 * Returns 0 on success, -1 if the registry is full (default cap: 64 entries).
 */
int  spu_register_ppu_fallback(uint32_t entry_point,
                               spu_ppu_fallback_fn handler, void* user);

/*
 * Unregister a previously-registered fallback. Idempotent. Returns 1 if a
 * matching entry was removed, 0 if no entry matched.
 */
int  spu_unregister_ppu_fallback(uint32_t entry_point);

/*
 * Internal: look up the fallback for an SPU image entry point. Used by the
 * lv2 SPU thread group implementation. Returns NULL if no match.
 */
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry_point,
                                            void** out_user);

/*
 * Get a pointer to a SPU thread's virtual local store (256 KB). Lazily
 * allocated on first read/write. Returns NULL if tid is unknown.
 *
 * Safe to call from a PPU fallback handler — typical usage is a job that
 * the PPU populated via sys_spu_thread_write_ls before group_start, so
 * the fallback reads its inputs from LS, computes results, and writes
 * outputs back. The PPU then reads them via sys_spu_thread_read_ls.
 */
uint8_t* spu_thread_get_local_store(uint32_t tid);

/* Local store size in bytes (always 256 KB to match real SPU). */
uint32_t spu_thread_local_store_size(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_SPU_FALLBACK_H */
