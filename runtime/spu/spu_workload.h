/* spu_workload.h — SPU workload / task dispatch registry.
 *
 * The bridge cellSpurs needs: when a title registers a SPURS workload
 * (cellSpursAddWorkload, a "policy module" SPU binary) or a SPURS task
 * (cellSpursCreateTask, a task ELF), the runtime must actually RUN that SPU
 * program. We do not interpret SPU ELFs at runtime — we statically lift each
 * title's SPU binaries ahead of time (spu_lifter -> a native entry fn). This
 * layer maps a registered SPU image, by content FINGERPRINT, to its pre-lifted
 * native entry, loads the image into a fresh 256 KB local store, and runs it
 * with the SPURS task ABI (job/task arg EA in r3) via the lifted-job adapter.
 *
 * Flow:
 *   1) At init, the title's generated registration code calls
 *      spu_workload_register(fingerprint, lifted_entry, "name") once per lifted
 *      SPU binary. The fingerprint is FNV-1a-64 over the exact ELF image bytes
 *      (the same bytes the title later hands to cellSpurs), computed identically
 *      here and by the offline extractor (so a manifest can be emitted).
 *   2) cellSpursAddWorkload / cellSpursCreateTask call spu_workload_dispatch()
 *      with the guest image -> fingerprint -> lifted entry -> load LS -> run.
 *
 * This is title-agnostic ps3recomp runtime: the registry is populated by the
 * title's lifted SPU set; the dispatch mechanism is generic.
 */
#ifndef SPU_WORKLOAD_H
#define SPU_WORKLOAD_H

#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* FNV-1a 64-bit over a byte range. The canonical workload fingerprint; the
 * offline extractor computes the same value so registrations and the images a
 * title passes to cellSpurs line up. */
uint64_t spu_workload_fingerprint(const void* data, size_t n);

/* Register one lifted SPU binary under its image fingerprint. `name` is borrowed
 * (kept by pointer for logging — pass a string literal / static). Idempotent on
 * fingerprint: a second register of the same fp updates the entry. */
void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name);

/* Look up a lifted entry by fingerprint; NULL if none registered. */
spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint);

/* Load a 32-bit big-endian SPU ELF image into a 256 KB local store: each PT_LOAD
 * segment is copied to its p_vaddr (BSS zero-filled). Returns 1 on success and
 * writes the entry vaddr (e_entry, which may legitimately be 0) to *entry_out;
 * returns 0 if `image` is not a valid SPU ELF or a segment is out of range.
 * `ls` must point to SPU_LS_SIZE bytes (caller-zeroed if a clean BSS is wanted). */
int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out);

/* Compute the true on-disk size of a 32-bit BE SPU ELF: the extent that spans
 * the section-header table and all program/section content (max p_offset+p_filesz
 * and non-NOBITS sh_offset+sh_size). This matches the offline extractor's sizing,
 * so fingerprints agree. Use it when a caller (e.g. cellSpursCreateTask) is handed
 * an SPU ELF pointer without an explicit length. `max_avail` bounds the read in
 * case the header is corrupt; returns 0 if `image` is not a valid SPU ELF. */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail);

/* Dispatch a registered SPU image: fingerprint `image`, find its lifted entry,
 * load it into a fresh local store, and run it with `args_ea` in r3 (SPURS task
 * ABI). Returns 1 if an image matched and ran, 0 if no lifted entry is
 * registered for it (caller may fall back to logging/stubbing). */
int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea);

/* Number of currently registered lifted SPU binaries (diagnostics/tests). */
unsigned spu_workload_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_WORKLOAD_H */
