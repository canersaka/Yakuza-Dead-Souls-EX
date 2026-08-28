#ifndef RSX_NR_RESIDENCY_H
#define RSX_NR_RESIDENCY_H

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
typedef std::atomic<uint32_t> rsx_nr_atomic_u32;
typedef std::atomic<uint64_t> rsx_nr_atomic_u64;
#else
#include <stdatomic.h>
typedef _Atomic uint32_t rsx_nr_atomic_u32;
typedef _Atomic uint64_t rsx_nr_atomic_u64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NR_RESIDENCY_DIRTY_WORDS 12u

typedef enum rsx_nr_residency_state {
    RSX_NR_RESIDENCY_IDLE = 0,
    RSX_NR_RESIDENCY_GPU_PENDING = 1,
    RSX_NR_RESIDENCY_MATERIALIZING = 2,
    RSX_NR_RESIDENCY_GUEST_COHERENT = 3,
    RSX_NR_RESIDENCY_GUEST_DIRTY = 4
} rsx_nr_residency_state;

typedef struct rsx_nr_residency_slot {
    rsx_nr_atomic_u32 state;
    rsx_nr_atomic_u32 generation;
    rsx_nr_atomic_u32 reset_generation;
    rsx_nr_atomic_u32 guest_ea;
    rsx_nr_atomic_u32 guest_size;
    rsx_nr_atomic_u64 writer_fence;
    rsx_nr_atomic_u64 dirty_pages[RSX_NR_RESIDENCY_DIRTY_WORDS];
    rsx_nr_atomic_u64 access_count;
    rsx_nr_atomic_u64 write_count;
} rsx_nr_residency_slot;

void rsx_nr_residency_init(rsx_nr_residency_slot* slot);
int rsx_nr_residency_begin(rsx_nr_residency_slot* slot, uint32_t ea,
                           uint32_t size, uint64_t writer_fence,
                           uint32_t* generation);
/* Returns the active generation, or zero for an unrelated/idle access.
 * need_materialize is true for both the thread which claims the transition
 * and peers which must wait for that same generation to become coherent. */
uint32_t rsx_nr_residency_access(rsx_nr_residency_slot* slot,
                                 uint32_t ea, uint32_t size, int write,
                                 int* need_materialize);
int rsx_nr_residency_mark_coherent(rsx_nr_residency_slot* slot,
                                   uint32_t generation);
int rsx_nr_residency_mark_dirty(rsx_nr_residency_slot* slot,
                                uint32_t generation,
                                uint32_t ea, uint32_t size);
int rsx_nr_residency_finish(rsx_nr_residency_slot* slot,
                            uint32_t generation);
void rsx_nr_residency_reset(rsx_nr_residency_slot* slot);

#ifdef __cplusplus
}
#endif
#endif
