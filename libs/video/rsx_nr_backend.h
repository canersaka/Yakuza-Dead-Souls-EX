/*
 * ps3recomp - native-render backend core: the ordered consumer of the
 * typed submission ring.
 *
 * Pops rsx_nir ops in submission order, folds state groups into a running
 * rsx_nir_pipeline, and executes actions through a pluggable execution
 * vtable (rsx_nr_exec_ops). GPU work (clear/draw/transfer/present/flush)
 * goes to the sink; host-visible synchronization (semaphores, reports,
 * SET_REFERENCE, user commands, tokens) is interpreted here with the
 * host side abstracted behind callbacks, so the whole executor is
 * offline-testable with an array-backed label window and a recording sink.
 *
 * Blocking is cooperative: rsx_nr_backend_step() never spins. An
 * unsatisfied SEMAPHORE_ACQUIRE or TOKEN_WAIT leaves the op at the ring
 * head and returns BLOCKED_*; the live consumer thread re-steps after its
 * usual wait discipline, and offline tests assert the block/unblock
 * transitions explicitly. Ops are never skipped and never execute out of
 * order; a sink failure is counted (exec_errors) and surfaces through the
 * coverage counters as an equivalence failure — never silent.
 */

#ifndef PS3RECOMP_RSX_NR_BACKEND_H
#define PS3RECOMP_RSX_NR_BACKEND_H

#include "rsx_nr_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rsx_nr_step_result {
    RSX_NR_STEP_EMPTY = 0,          /* ring drained                        */
    RSX_NR_STEP_EXECUTED,           /* one op consumed                     */
    RSX_NR_STEP_BLOCKED_TOKEN,      /* head is an unsatisfied TOKEN_WAIT   */
    RSX_NR_STEP_BLOCKED_SEMAPHORE,  /* head is an unsatisfied acquire      */
} rsx_nr_step_result;

/* Every callback may be NULL (treated as success / value 0). */
typedef struct rsx_nr_exec_ops {
    void* user;
    int  (*clear)(void* u, const rsx_nir_pipeline* st,
                  const rsx_nir_clear* c);
    int  (*draw)(void* u, const rsx_nir_pipeline* st,
                 const u32* vp_words, u32 vp_word_count,
                 const rsx_nir_draw* d, const u32* batches);
    int  (*transfer)(void* u, const rsx_nir_pipeline* st,
                     const rsx_nir_transfer* t, const u32* words);
    int  (*present)(void* u, u32 buffer);
    void (*flush)(void* u);                       /* barrier / pre-REF     */
    /* value is the FINAL memory value: the core has already applied the
     * hardware store transform (back-end releases swizzle bytes 0<->2;
     * texture-pipe releases are verbatim — see rsx_nir_semaphore). */
    void (*sem_write)(void* u, u32 dma, u32 offset, u32 value,
                      u32 texture_read);
    int  (*sem_read)(void* u, u32 dma, u32 offset, u32* value);
    int (*report)(void* u, u32 kind, u32 arg, u32 dma);
    void (*set_reference)(void* u, u32 value);
    void (*user_command)(void* u, u32 cause);
} rsx_nr_exec_ops;

typedef struct rsx_nr_backend_stats {
    unsigned long long executed[RSX_NIR_OP_KIND_COUNT];
    unsigned long long blocked_token, blocked_semaphore;
    unsigned long long exec_errors;
    unsigned long long fallback_enters, fallback_exits;
} rsx_nr_backend_stats;

typedef struct rsx_nr_backend {
    rsx_nr_ring* ring;
    rsx_nr_tokens* tokens;
    rsx_nr_exec_ops ops;
    rsx_nir_pipeline st;
    u32 vp_words[RSX_NIR_VP_MAX_WORDS];  /* current program content        */
    u32 vp_word_count;
    rsx_nr_backend_stats stats;
} rsx_nr_backend;

void rsx_nr_backend_init(rsx_nr_backend* be, rsx_nr_ring* ring,
                         rsx_nr_tokens* tokens, const rsx_nr_exec_ops* ops);

/* Execute at most one op (see blocking contract above). */
rsx_nr_step_result rsx_nr_backend_step(rsx_nr_backend* be);

/* Step until blocked or empty (at most max_ops when nonzero). Returns the
 * number of ops executed. */
u32 rsx_nr_backend_run(rsx_nr_backend* be, u32 max_ops);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_NR_BACKEND_H */
