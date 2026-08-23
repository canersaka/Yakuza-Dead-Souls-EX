/* spu_njs -- native job scheduler foundation (offline-testable, default-off).
 *
 * Stage-4 groundwork of docs/SPU_WB_ROADMAP.md: typed extraction of legacy
 * SPURS job descriptors, the exact run_job local-store construction contract
 * as a pure reusable function, and a dependency/barrier scheduler that
 * dispatches typed native job functions through a pluggable executor.
 *
 * ZERO live consumers: nothing in the game links this translation unit
 * unless YZ_SPU_NJS is set at configure time; the only current user is
 * tests/spu_njs. Every parsing/layout rule is transcribed from the live
 * jobchain implementation (libs/spurs/cellSpurs.c run_job, cited per rule)
 * so a later integration can hand descriptors to this scheduler behind a
 * runtime gate without re-deriving the contract.
 *
 * Semantics preserved by design (and pinned by tests/spu_njs):
 *   - descriptor validation identical to run_job (sizes, alignment,
 *     jobType==0 only, dma/cache list bounds, MFC stall-and-notify flag,
 *     sub-quadword element sizes/alignment);
 *   - LS layout identical to run_job (slot alternation with fit fallback,
 *     context block 0x4940, descriptor copy 0x3F000, 1KB-aligned io/out/
 *     cache/scratch+stack allocations, sub-quadword low-nibble placement,
 *     SPU ABI 2.5.1 entry stack frame, r0=0x0A70 task-syscall link,
 *     r3=context, r4=descriptor, alternating DMA tags);
 *   - scheduling: in-order command consumption; jobs between barriers may
 *     run concurrently (each job publishes its own outputs via its DMA,
 *     the hardware contract); SYNC/LWSYNC drain every prior job; ABORT
 *     stops issuing and drains in-flight; END drains and terminates.
 */

#ifndef SPU_NJS_H
#define SPU_NJS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- typed job record -------------------------------------------------- */

#define SPU_NJS_MAX_IO     118           /* (0x400-0x30)/8 */
#define SPU_NJS_MAX_CACHE  4

typedef struct spu_njs_io_item {
    uint32_t ea;
    uint32_t size;                        /* 15-bit MFC list size */
    uint32_t stall_notify;                /* top bit of the element */
} spu_njs_io_item;

typedef struct spu_njs_job {
    uint32_t descriptor_ea;
    uint32_t descriptor_size;
    uint8_t  descriptor[0x400];           /* acquired snapshot */
    uint32_t bin_ea, bin_size;
    uint32_t kind;                        /* d+0x10 (useInOutBuffer / ring kind) */
    uint32_t size_io, size_out, size_stack, size_scratch;
    uint32_t io_count;
    spu_njs_io_item io[SPU_NJS_MAX_IO];
    uint32_t cache_count;
    uint32_t cache_ea[SPU_NJS_MAX_CACHE];
    uint32_t cache_size[SPU_NJS_MAX_CACHE];
    uint64_t sequence;                    /* submission order, scheduler-set */
} spu_njs_job;

/* parse errors mirror run_job's rejection classes */
typedef enum spu_njs_status {
    SPU_NJS_OK = 0,
    SPU_NJS_E_DESCRIPTOR,                 /* CELL_SPURS_JOB_ERROR_DESCRIPTOR */
    SPU_NJS_E_BINARY2,                    /* jobType != 0: format unknown */
    SPU_NJS_E_INVALID_BIN,
    SPU_NJS_E_NOMEM,
    SPU_NJS_E_FAULT,
    SPU_NJS_E_STATE,
} spu_njs_status;

/* Parse + validate a legacy job descriptor snapshot into a typed record.
 * Every rule cites run_job (cellSpurs.c). Does not touch guest memory. */
spu_njs_status spu_njs_parse_descriptor(const uint8_t* descriptor,
                                        uint32_t descriptor_size,
                                        uint32_t descriptor_ea,
                                        spu_njs_job* out);

/* ---- local-store construction ------------------------------------------ */

/* Guest memory read: copy [ea, ea+size) into dst; return 0 on fault. */
typedef int (*spu_njs_guest_read_fn)(void* user, uint32_t ea,
                                     void* dst, uint32_t size);

typedef struct spu_njs_ls_init {
    uint32_t slot;                        /* chosen binary LS base */
    uint32_t pc;                          /* == slot */
    uint32_t r0;                          /* 0x0A70 task-syscall link */
    uint32_t r1_sp;                       /* SPU ABI initial stack pointer */
    uint32_t r1_avail;                    /* ABI word 1: available stack */
    uint32_t r3;                          /* context LS */
    uint32_t r4;                          /* descriptor LS */
    uint32_t io_ls, out_ls, scratch_stack_ls;
    uint32_t cache_ls[SPU_NJS_MAX_CACHE];
    uint32_t dma_tag;
} spu_njs_ls_init;

/* Build the job's 256KB local store exactly as run_job does. `ls` must be
 * SPU_LS_SIZE bytes. preferred_slot in {0x4C00, 0xE400}; the other slot is
 * the fit fallback. dma_tag_pair supplies the jobchain's two tags; parity
 * selects which (run_job alternates). Returns SPU_NJS_OK or the run_job
 * rejection class. */
spu_njs_status spu_njs_build_ls(const spu_njs_job* job,
                                uint8_t* ls,
                                uint32_t preferred_slot,
                                const uint32_t dma_tag_pair[2],
                                uint32_t tag_parity,
                                spu_njs_guest_read_fn guest_read,
                                void* guest_user,
                                spu_njs_ls_init* init);

/* ---- dependency / barrier scheduler ------------------------------------ */

typedef struct spu_njs_sched spu_njs_sched;

/* Execute one job to completion (including its own output DMA -- the
 * hardware contract: a job's guest-visible outputs are ITS DMA, and its
 * completion publication is part of that output). Returns nonzero ok. */
typedef int (*spu_njs_exec_fn)(void* user, const spu_njs_job* job);
/* Completion hook, called once per job in COMPLETION order (matching the
 * hardware's publication model; ordering ACROSS barriers is guaranteed). */
typedef void (*spu_njs_complete_fn)(void* user, const spu_njs_job* job,
                                    int ok);

/* width == 0 or 1: deterministic sequential mode (submission order).
 * width > 1: a Win32 worker pool; jobs between barriers run concurrently. */
spu_njs_sched* spu_njs_sched_create(unsigned width,
                                    spu_njs_exec_fn exec,
                                    spu_njs_complete_fn complete,
                                    void* user);
void spu_njs_sched_destroy(spu_njs_sched* s);

/* In-order command stream. All return SPU_NJS_OK, or SPU_NJS_E_STATE after
 * END/ABORT. */
spu_njs_status spu_njs_submit(spu_njs_sched* s, const spu_njs_job* job);
/* SYNC/LWSYNC: every previously submitted job has completed (its exec
 * returned => its output DMA is done) before this returns. */
spu_njs_status spu_njs_sync(spu_njs_sched* s);
/* ABORT: no further submissions accepted; in-flight jobs drain; returns
 * after the drain. Already-completed and in-flight jobs keep their
 * publications (a job's own DMA cannot be recalled -- hardware contract). */
spu_njs_status spu_njs_abort(spu_njs_sched* s);
/* END: drain everything, mark terminal. */
spu_njs_status spu_njs_end(spu_njs_sched* s);

/* introspection for tests */
uint64_t spu_njs_completed(const spu_njs_sched* s);
uint64_t spu_njs_submitted(const spu_njs_sched* s);
int spu_njs_terminal(const spu_njs_sched* s);   /* 0 live, 1 ended, 2 aborted */

#ifdef __cplusplus
}
#endif

#endif /* SPU_NJS_H */
