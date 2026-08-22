/*
 * Exact guest-FIFO-address router for producer-boundary typed operations.
 *
 * A recognized GCM wrapper can replace its packet only after this router has
 * accepted the typed payload for the exact guest command address.  The live
 * FIFO consumer performs an atomic watched-page rejection, claims a matching
 * span, executes its typed payload, and advances GET by .word_count.  A miss
 * never guesses and never consumes guest words.
 *
 * Storage is allocated once by init and remains fixed until destroy.  There
 * is no allocation, clock, scan, or I/O on publish/take.  Saturation and
 * producer contention fail closed so the wrapper can run its lifted packet
 * path before advancing the guest command context.
 */
#ifndef PS3RECOMP_RSX_NR_SPAN_ROUTER_H
#define PS3RECOMP_RSX_NR_SPAN_ROUTER_H

#include "rsx_nir.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NR_SPAN_MAX_OPS       4u
#define RSX_NR_SPAN_MAX_SIDE      32u

typedef struct rsx_nr_span_payload {
    u32 op_count;
    u32 side_count;
    rsx_nir_op ops[RSX_NR_SPAN_MAX_OPS];
    u32 side[RSX_NR_SPAN_MAX_SIDE];
} rsx_nr_span_payload;

typedef struct rsx_nr_span {
    u32 ea;                    /* exact first guest FIFO byte              */
    u32 word_count;            /* guest span replaced by this payload     */
    u32 generation;            /* command-context/reset generation        */
    u32 fingerprint;           /* payload integrity + copied-list guard   */
    rsx_nr_span_payload payload;
} rsx_nr_span;

typedef enum rsx_nr_span_publish_result {
    RSX_NR_SPAN_PUBLISHED = 0,
    RSX_NR_SPAN_PUBLISH_INVALID,
    RSX_NR_SPAN_PUBLISH_DUPLICATE,
    RSX_NR_SPAN_PUBLISH_BUSY,
    RSX_NR_SPAN_PUBLISH_FULL,
} rsx_nr_span_publish_result;

typedef enum rsx_nr_span_take_result {
    RSX_NR_SPAN_TAKE_FAST_MISS = 0, /* watched-page bit was clear           */
    RSX_NR_SPAN_TAKE_MISS,          /* watched page, no exact ready span    */
    RSX_NR_SPAN_TAKE_NOT_READY,     /* publication at a probed slot active  */
    RSX_NR_SPAN_TAKE_CLAIMED,
    RSX_NR_SPAN_TAKE_CORRUPT,
} rsx_nr_span_take_result;

typedef struct rsx_nr_span_router_stats {
    unsigned long long published;
    unsigned long long claimed;
    unsigned long long fast_misses;
    unsigned long long exact_misses;
    unsigned long long not_ready;
    unsigned long long duplicates;
    unsigned long long busy;
    unsigned long long full;
    unsigned long long corrupt;
} rsx_nr_span_router_stats;

typedef struct rsx_nr_span_router {
    void* slots;               /* private fixed slot array                 */
    void* page_counts;         /* exact atomic count per 4 KiB guest page  */
    u32 capacity;              /* power of two                             */
    u32 generation;
    u32 publication_epoch;     /* changes after every ready publication    */
    u32 count_misses;          /* diagnostic counters; hot path may disable*/
    volatile long producer_lock;
    volatile long long counters[9];
} rsx_nr_span_router;

/* capacity must be a power of two and at least 8. */
int  rsx_nr_span_router_init(rsx_nr_span_router* r, u32 capacity);
void rsx_nr_span_router_destroy(rsx_nr_span_router* r);

/* Quiescent reset only: caller has stopped all publishers and the consumer.
 * Invalidates every old span and returns the new nonzero generation. */
u32 rsx_nr_span_router_reset(rsx_nr_span_router* r);
u32 rsx_nr_span_router_generation(const rsx_nr_span_router* r);

/* Monotonic cache-invalidation epoch. A single consumer may memoize an exact
 * miss at (ea, epoch); it must retry after this value changes. */
u32 rsx_nr_span_router_publication_epoch(const rsx_nr_span_router* r);
void rsx_nr_span_router_set_miss_counting(rsx_nr_span_router* r, int enabled);

/* Publish copies the payload. .ea must be word-aligned, .word_count nonzero,
 * and .generation equal to the router's current generation. */
rsx_nr_span_publish_result
rsx_nr_span_router_publish(rsx_nr_span_router* r, const rsx_nr_span* span);

/* Exact, destructive claim.  On CLAIMED, *out owns a complete copy and the
 * slot is immediately recyclable.  The caller executes that payload once. */
rsx_nr_span_take_result
rsx_nr_span_router_take(rsx_nr_span_router* r, u32 ea, rsx_nr_span* out);

/* Deterministic fingerprint over address/length/generation and the used
 * typed ops/side words.  Exposed for producer templates and tests. */
u32 rsx_nr_span_fingerprint(const rsx_nr_span* span);

void rsx_nr_span_router_get_stats(const rsx_nr_span_router* r,
                                  rsx_nr_span_router_stats* out);

#ifdef __cplusplus
}
#endif
#endif
