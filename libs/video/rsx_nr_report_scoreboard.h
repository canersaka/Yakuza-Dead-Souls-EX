/*
 * Fixed-memory deferred RSX report publication.
 *
 * GET_REPORT is an ordered GPU command, but it is not a reason by itself to
 * retire the complete command list.  This scoreboard retains the synthetic
 * guest-visible payload until the fence which contains the report completes.
 * A proven early reader may request that fence; ordinary reports retire in
 * command order at an existing submission boundary.
 */
#ifndef PS3RECOMP_RSX_NR_REPORT_SCOREBOARD_H
#define PS3RECOMP_RSX_NR_REPORT_SCOREBOARD_H

#include "rsx_nir.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RSX_NR_REPORT_PENDING_CAPACITY = 4096,
    RSX_NR_REPORT_FAMILY_CAPACITY = 512,
};

typedef enum rsx_nr_report_read_source {
    RSX_NR_REPORT_READ_PPU = 0,
    RSX_NR_REPORT_READ_SPU,
    RSX_NR_REPORT_READ_RSX_CONDITION,
    RSX_NR_REPORT_READ_HLE,
    RSX_NR_REPORT_READ_SOURCE_COUNT
} rsx_nr_report_read_source;

typedef enum rsx_nr_report_fallback_reason {
    RSX_NR_REPORT_FALLBACK_DISABLED = 0,
    RSX_NR_REPORT_FALLBACK_CLEAR_UNMODELED,
    RSX_NR_REPORT_FALLBACK_BAD_DMA,
    RSX_NR_REPORT_FALLBACK_BAD_RANGE,
    RSX_NR_REPORT_FALLBACK_NO_TIMELINE,
    RSX_NR_REPORT_FALLBACK_CAPACITY,
    RSX_NR_REPORT_FALLBACK_UNKNOWN_TYPE,
    RSX_NR_REPORT_FALLBACK_SUBMIT_FAILED,
    RSX_NR_REPORT_FALLBACK_PUBLISH_FAILED,
    RSX_NR_REPORT_FALLBACK_REASON_COUNT
} rsx_nr_report_fallback_reason;

typedef enum rsx_nr_report_publication_state {
    RSX_NR_REPORT_PENDING_UNSUBMITTED = 0,
    RSX_NR_REPORT_PENDING_SUBMITTED,
    RSX_NR_REPORT_PUBLISHED,
    RSX_NR_REPORT_ABANDONED
} rsx_nr_report_publication_state;

typedef struct rsx_nr_report_desc {
    u32 kind;
    u32 type;
    u32 dma;
    u32 offset;
    u32 ea;
    u32 query_slot;
    u32 guest_value;
    u32 guest_value_valid;
    u64 writer_generation;
    u64 recording_fence;
} rsx_nr_report_desc;

typedef struct rsx_nr_report_record {
    u64 sequence;
    rsx_nr_report_desc desc;
    u64 submitted_fence;
    u64 reset_generation;
    u32 publication_state;
    u32 family_slot;
} rsx_nr_report_record;

typedef struct rsx_nr_report_family_stats {
    u32 occupied;
    u32 kind, type, dma, offset, ea, query_slot;
    u64 produced;
    u64 deferred;
    u64 published_natural;
    u64 published_early;
    u64 overwritten_pending;
    u64 first_sequence;
    u64 first_consumer_sequence;
    u64 read_count[RSX_NR_REPORT_READ_SOURCE_COUNT];
    u64 fallback[RSX_NR_REPORT_FALLBACK_REASON_COUNT];
} rsx_nr_report_family_stats;

typedef struct rsx_nr_report_scoreboard_stats {
    u64 produced;
    u64 deferred;
    u64 clear_noops;
    u64 natural_submissions;
    u64 early_submissions;
    u64 reports_published;
    u64 reports_published_natural;
    u64 reports_published_early;
    u64 reader_checks;
    u64 early_consumer_hits;
    u64 pending_high_water;
    u64 reset_count;
    u64 shutdown_count;
    u64 abandoned;
    u64 fallback[RSX_NR_REPORT_FALLBACK_REASON_COUNT];
} rsx_nr_report_scoreboard_stats;

typedef u64 (*rsx_nr_report_timestamp_fn)(void* user);
typedef int (*rsx_nr_report_publish_fn)(
    void* user, const rsx_nr_report_record* record, u64 timestamp);
/* Submit/wait the list containing required_fence.  On success return zero and
 * write the actually completed fence. */
typedef int (*rsx_nr_report_submit_wait_fn)(
    void* user, u64 required_fence, u64* completed_fence);

typedef struct rsx_nr_report_scoreboard_ops {
    void* user;
    rsx_nr_report_timestamp_fn timestamp;
    rsx_nr_report_publish_fn publish;
    rsx_nr_report_submit_wait_fn submit_wait;
} rsx_nr_report_scoreboard_ops;

typedef struct rsx_nr_report_scoreboard {
    /* Public ABI uses a plain 32-bit word so this fixed-memory structure has
     * identical layout in C and C++. The C implementation performs C11
     * atomic operations through this word. */
    volatile u32 lock;
    u32 enabled;
    u32 head;
    u32 count;
    u32 reserved;
    u64 next_sequence;
    u64 reset_generation;
    u64 completed_fence;
    rsx_nr_report_scoreboard_ops ops;
    rsx_nr_report_record pending[RSX_NR_REPORT_PENDING_CAPACITY];
    rsx_nr_report_family_stats family[RSX_NR_REPORT_FAMILY_CAPACITY];
    rsx_nr_report_scoreboard_stats stats;
} rsx_nr_report_scoreboard;

void rsx_nr_report_scoreboard_init(
    rsx_nr_report_scoreboard* sb, int enabled,
    const rsx_nr_report_scoreboard_ops* ops);

/* Zero means deferred. Positive means preserve the immediate legacy/current
 * path and identifies a rsx_nr_report_fallback_reason. */
int rsx_nr_report_scoreboard_enqueue(
    rsx_nr_report_scoreboard* sb, const rsx_nr_report_desc* desc);
void rsx_nr_report_scoreboard_note_clear_noop(
    rsx_nr_report_scoreboard* sb, const rsx_nr_report_desc* desc);
void rsx_nr_report_scoreboard_note_fallback(
    rsx_nr_report_scoreboard* sb, const rsx_nr_report_desc* desc,
    rsx_nr_report_fallback_reason reason);

/* The caller has synchronously completed a natural submission. */
int rsx_nr_report_scoreboard_complete(
    rsx_nr_report_scoreboard* sb, u64 submitted_fence,
    u64 completed_fence, int natural);

/* Called before bytes in [ea,ea+size) become visible to a proven consumer. */
int rsx_nr_report_scoreboard_consume(
    rsx_nr_report_scoreboard* sb, u32 ea, u32 size,
    rsx_nr_report_read_source source);

/* Reset/shutdown are fail-closed: pending records must already have been
 * completed, or they are counted as abandoned rather than published early. */
void rsx_nr_report_scoreboard_reset(rsx_nr_report_scoreboard* sb);
void rsx_nr_report_scoreboard_shutdown(rsx_nr_report_scoreboard* sb);

u32 rsx_nr_report_scoreboard_pending(const rsx_nr_report_scoreboard* sb);
void rsx_nr_report_scoreboard_get_stats(
    const rsx_nr_report_scoreboard* sb,
    rsx_nr_report_scoreboard_stats* out);
u32 rsx_nr_report_scoreboard_get_families(
    const rsx_nr_report_scoreboard* sb,
    rsx_nr_report_family_stats* out, u32 capacity);

#ifdef __cplusplus
}
#endif
#endif
