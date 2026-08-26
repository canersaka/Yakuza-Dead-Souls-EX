#include "rsx_nr_report_scoreboard.h"

#include <stdatomic.h>
#include <string.h>

static void sb_lock(rsx_nr_report_scoreboard* sb)
{
    while (atomic_exchange_explicit(
               (_Atomic u32*)&sb->lock, 1u, memory_order_acquire))
        ;
}

static void sb_unlock(rsx_nr_report_scoreboard* sb)
{
    atomic_store_explicit(
        (_Atomic u32*)&sb->lock, 0u, memory_order_release);
}

static u32 sb_hash(const rsx_nr_report_desc* d)
{
    u32 h = 2166136261u;
#define SB_MIX(v) do { h ^= (u32)(v); h *= 16777619u; } while (0)
    SB_MIX(d->kind); SB_MIX(d->type); SB_MIX(d->dma);
    SB_MIX(d->offset); SB_MIX(d->ea); SB_MIX(d->query_slot);
#undef SB_MIX
    return h;
}

static int sb_family_equal(const rsx_nr_report_family_stats* f,
                           const rsx_nr_report_desc* d)
{
    return f->kind == d->kind && f->type == d->type &&
           f->dma == d->dma && f->offset == d->offset &&
           f->ea == d->ea && f->query_slot == d->query_slot;
}

static u32 sb_family_slot(rsx_nr_report_scoreboard* sb,
                          const rsx_nr_report_desc* d, int create)
{
    u32 slot = sb_hash(d) & (RSX_NR_REPORT_FAMILY_CAPACITY - 1u);
    for (u32 probe = 0; probe < RSX_NR_REPORT_FAMILY_CAPACITY; ++probe) {
        rsx_nr_report_family_stats* const f = &sb->family[slot];
        if (!f->occupied) {
            if (!create)
                return UINT32_MAX;
            memset(f, 0, sizeof(*f));
            f->occupied = 1u;
            f->kind = d->kind; f->type = d->type; f->dma = d->dma;
            f->offset = d->offset; f->ea = d->ea;
            f->query_slot = d->query_slot;
            return slot;
        }
        if (sb_family_equal(f, d))
            return slot;
        slot = (slot + 1u) & (RSX_NR_REPORT_FAMILY_CAPACITY - 1u);
    }
    return UINT32_MAX;
}

static int sb_overlap(u32 a, u32 an, u32 b, u32 bn)
{
    if (!an || !bn)
        return 0;
    const u64 ae = (u64)a + an;
    const u64 be = (u64)b + bn;
    return (u64)a < be && (u64)b < ae;
}

void rsx_nr_report_scoreboard_init(
    rsx_nr_report_scoreboard* sb, int enabled,
    const rsx_nr_report_scoreboard_ops* ops)
{
    memset(sb, 0, sizeof(*sb));
    sb->enabled = enabled ? 1u : 0u;
    sb->next_sequence = 1u;
    sb->reset_generation = 1u;
    if (ops)
        sb->ops = *ops;
}

void rsx_nr_report_scoreboard_note_fallback(
    rsx_nr_report_scoreboard* sb, const rsx_nr_report_desc* desc,
    rsx_nr_report_fallback_reason reason)
{
    if (!sb || !desc || (u32)reason >= RSX_NR_REPORT_FALLBACK_REASON_COUNT)
        return;
    sb_lock(sb);
    sb->stats.fallback[reason]++;
    const u32 slot = sb_family_slot(sb, desc, 1);
    if (slot != UINT32_MAX)
        sb->family[slot].fallback[reason]++;
    sb_unlock(sb);
}

int rsx_nr_report_scoreboard_enqueue(
    rsx_nr_report_scoreboard* sb, const rsx_nr_report_desc* desc)
{
    if (!sb || !desc)
        return RSX_NR_REPORT_FALLBACK_BAD_RANGE;
    if (!sb->enabled) {
        rsx_nr_report_scoreboard_note_fallback(
            sb, desc, RSX_NR_REPORT_FALLBACK_DISABLED);
        return RSX_NR_REPORT_FALLBACK_DISABLED;
    }
    if (!desc->recording_fence) {
        rsx_nr_report_scoreboard_note_fallback(
            sb, desc, RSX_NR_REPORT_FALLBACK_NO_TIMELINE);
        return RSX_NR_REPORT_FALLBACK_NO_TIMELINE;
    }

    sb_lock(sb);
    sb->stats.produced++;
    const u32 family = sb_family_slot(sb, desc, 1);
    if (family == UINT32_MAX || sb->count >= RSX_NR_REPORT_PENDING_CAPACITY) {
        sb->stats.fallback[RSX_NR_REPORT_FALLBACK_CAPACITY]++;
        if (family != UINT32_MAX)
            sb->family[family].fallback[RSX_NR_REPORT_FALLBACK_CAPACITY]++;
        sb_unlock(sb);
        return RSX_NR_REPORT_FALLBACK_CAPACITY;
    }

    rsx_nr_report_family_stats* const fs = &sb->family[family];
    fs->produced++;
    fs->deferred++;
    if (!fs->first_sequence)
        fs->first_sequence = sb->next_sequence;
    for (u32 i = 0; i < sb->count; ++i) {
        const u32 at = (sb->head + i) % RSX_NR_REPORT_PENDING_CAPACITY;
        const rsx_nr_report_record* const old = &sb->pending[at];
        if (old->publication_state < RSX_NR_REPORT_PUBLISHED &&
            sb_overlap(old->desc.ea, 16u, desc->ea, 16u))
            fs->overwritten_pending++;
    }

    const u32 tail = (sb->head + sb->count) %
        RSX_NR_REPORT_PENDING_CAPACITY;
    rsx_nr_report_record* const record = &sb->pending[tail];
    memset(record, 0, sizeof(*record));
    record->sequence = sb->next_sequence++;
    record->desc = *desc;
    record->reset_generation = sb->reset_generation;
    record->publication_state = RSX_NR_REPORT_PENDING_UNSUBMITTED;
    record->family_slot = family;
    sb->count++;
    sb->stats.deferred++;
    if (sb->count > sb->stats.pending_high_water)
        sb->stats.pending_high_water = sb->count;
    sb_unlock(sb);
    return 0;
}

void rsx_nr_report_scoreboard_note_clear_noop(
    rsx_nr_report_scoreboard* sb, const rsx_nr_report_desc* desc)
{
    if (!sb || !desc || !sb->enabled)
        return;
    sb_lock(sb);
    sb->stats.produced++;
    sb->stats.deferred++;
    sb->stats.clear_noops++;
    const u32 family = sb_family_slot(sb, desc, 1);
    if (family != UINT32_MAX) {
        rsx_nr_report_family_stats* const f = &sb->family[family];
        f->produced++;
        f->deferred++;
        if (!f->first_sequence)
            f->first_sequence = sb->next_sequence;
    }
    sb->next_sequence++;
    sb_unlock(sb);
}

static int sb_publish_completed_locked(rsx_nr_report_scoreboard* sb,
                                       u64 completed_fence, int natural)
{
    int published = 0;
    while (sb->count) {
        rsx_nr_report_record* const r = &sb->pending[sb->head];
        if (r->desc.recording_fence > completed_fence)
            break;
        if (!r->submitted_fence)
            r->submitted_fence = r->desc.recording_fence;
        r->publication_state = RSX_NR_REPORT_PENDING_SUBMITTED;
        const u64 timestamp = sb->ops.timestamp
            ? sb->ops.timestamp(sb->ops.user) : 0u;
        if (sb->ops.publish &&
            sb->ops.publish(sb->ops.user, r, timestamp) != 0) {
            sb->stats.fallback[RSX_NR_REPORT_FALLBACK_PUBLISH_FAILED]++;
            sb->family[r->family_slot]
                .fallback[RSX_NR_REPORT_FALLBACK_PUBLISH_FAILED]++;
            return -1;
        }
        r->publication_state = RSX_NR_REPORT_PUBLISHED;
        rsx_nr_report_family_stats* const f = &sb->family[r->family_slot];
        if (natural)
            f->published_natural++;
        else
            f->published_early++;
        sb->stats.reports_published++;
        if (natural)
            sb->stats.reports_published_natural++;
        else
            sb->stats.reports_published_early++;
        sb->head = (sb->head + 1u) % RSX_NR_REPORT_PENDING_CAPACITY;
        sb->count--;
        published++;
    }
    if (completed_fence > sb->completed_fence)
        sb->completed_fence = completed_fence;
    return published;
}

int rsx_nr_report_scoreboard_complete(
    rsx_nr_report_scoreboard* sb, u64 submitted_fence,
    u64 completed_fence, int natural)
{
    if (!sb || !sb->enabled)
        return 0;
    sb_lock(sb);
    int covers_report = 0;
    for (u32 i = 0; i < sb->count; ++i) {
        rsx_nr_report_record* const r = &sb->pending[
            (sb->head + i) % RSX_NR_REPORT_PENDING_CAPACITY];
        if (r->desc.recording_fence <= submitted_fence)
            covers_report = 1;
        if (!r->submitted_fence &&
            r->desc.recording_fence <= submitted_fence) {
            r->submitted_fence = r->desc.recording_fence;
            r->publication_state = RSX_NR_REPORT_PENDING_SUBMITTED;
        }
    }
    if (covers_report) {
        if (natural)
            sb->stats.natural_submissions++;
        else
            sb->stats.early_submissions++;
    }
    const int result = sb_publish_completed_locked(sb, completed_fence, natural);
    sb_unlock(sb);
    return result;
}

int rsx_nr_report_scoreboard_consume(
    rsx_nr_report_scoreboard* sb, u32 ea, u32 size,
    rsx_nr_report_read_source source)
{
    if (!sb || !sb->enabled || !size ||
        (u32)source >= RSX_NR_REPORT_READ_SOURCE_COUNT)
        return 0;

    for (;;) {
        u64 required = 0;
        sb_lock(sb);
        sb->stats.reader_checks++;
        for (u32 i = 0; i < sb->count; ++i) {
            rsx_nr_report_record* const r = &sb->pending[
                (sb->head + i) % RSX_NR_REPORT_PENDING_CAPACITY];
            if (!sb_overlap(r->desc.ea, 16u, ea, size))
                continue;
            if (r->desc.recording_fence > required)
                required = r->desc.recording_fence;
            rsx_nr_report_family_stats* const f =
                &sb->family[r->family_slot];
            f->read_count[source]++;
            if (!f->first_consumer_sequence)
                f->first_consumer_sequence = sb->next_sequence;
        }
        if (!required) {
            sb_unlock(sb);
            return 0;
        }
        sb->stats.early_consumer_hits++;
        sb_unlock(sb);

        if (!sb->ops.submit_wait) {
            sb_lock(sb);
            sb->stats.fallback[RSX_NR_REPORT_FALLBACK_SUBMIT_FAILED]++;
            sb_unlock(sb);
            return -1;
        }
        u64 completed = 0;
        if (sb->ops.submit_wait(sb->ops.user, required, &completed) != 0 ||
            completed < required) {
            sb_lock(sb);
            sb->stats.fallback[RSX_NR_REPORT_FALLBACK_SUBMIT_FAILED]++;
            sb_unlock(sb);
            return -1;
        }
        if (rsx_nr_report_scoreboard_complete(
                sb, required, completed, 0) < 0)
            return -1;
    }
}

static void sb_abandon_locked(rsx_nr_report_scoreboard* sb)
{
    while (sb->count) {
        rsx_nr_report_record* const r = &sb->pending[sb->head];
        r->publication_state = RSX_NR_REPORT_ABANDONED;
        sb->stats.abandoned++;
        sb->head = (sb->head + 1u) % RSX_NR_REPORT_PENDING_CAPACITY;
        sb->count--;
    }
}

void rsx_nr_report_scoreboard_reset(rsx_nr_report_scoreboard* sb)
{
    if (!sb)
        return;
    sb_lock(sb);
    sb_abandon_locked(sb);
    sb->reset_generation++;
    sb->stats.reset_count++;
    sb_unlock(sb);
}

void rsx_nr_report_scoreboard_shutdown(rsx_nr_report_scoreboard* sb)
{
    if (!sb)
        return;
    sb_lock(sb);
    sb_abandon_locked(sb);
    sb->stats.shutdown_count++;
    sb->enabled = 0;
    sb_unlock(sb);
}

u32 rsx_nr_report_scoreboard_pending(const rsx_nr_report_scoreboard* sb)
{
    return sb ? sb->count : 0u;
}

void rsx_nr_report_scoreboard_get_stats(
    const rsx_nr_report_scoreboard* sb,
    rsx_nr_report_scoreboard_stats* out)
{
    if (!out)
        return;
    if (!sb) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = sb->stats;
}

u32 rsx_nr_report_scoreboard_get_families(
    const rsx_nr_report_scoreboard* sb,
    rsx_nr_report_family_stats* out, u32 capacity)
{
    if (!sb || !out || !capacity)
        return 0;
    u32 count = 0;
    for (u32 i = 0; i < RSX_NR_REPORT_FAMILY_CAPACITY && count < capacity;
         ++i) {
        if (sb->family[i].occupied)
            out[count++] = sb->family[i];
    }
    return count;
}
