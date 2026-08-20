#include "rsx_wait_classifier.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

constexpr uint64_t kBucketCount = 4096u;
constexpr DWORD kBucketMilliseconds = 1000u;
constexpr uint32_t kSemaphoreAggregateCount = 512u;
constexpr uint32_t kStopperAggregateCount = 512u;

struct Bucket {
    uint64_t second;
    uint64_t wall_ticks[YZ_RSX_WAIT_CATEGORY_COUNT];
    uint64_t transitions[YZ_RSX_WAIT_CATEGORY_COUNT];
    uint64_t polls[YZ_RSX_WAIT_CATEGORY_COUNT];
    uint64_t progressing_steps;
    uint64_t dispatched_methods;
    uint64_t completed_draws;
    uint64_t observed_put_changes;
    int valid;
};

struct Totals {
    uint64_t wall_ticks[YZ_RSX_WAIT_CATEGORY_COUNT];
    uint64_t transitions[YZ_RSX_WAIT_CATEGORY_COUNT];
    uint64_t polls[YZ_RSX_WAIT_CATEGORY_COUNT];
    uint64_t progressing_steps;
    uint64_t dispatched_methods;
    uint64_t completed_draws;
    uint64_t observed_put_changes;
};

struct SemaphoreAggregate {
    yz_rsx_semaphore_wait key;
    uint32_t first_entry_value;
    uint32_t last_entry_value;
    uint32_t last_exit_value;
    uint64_t total_ticks;
    uint64_t episode_count;
    uint64_t poll_count;
    uint64_t value_change_count;
    uint64_t first_second;
    uint64_t last_second;
    int valid;
};

struct StopperAggregate {
    yz_rsx_stopper_wait key;
    uint32_t first_entry_word;
    uint32_t last_entry_word;
    uint32_t last_exit_word;
    uint32_t first_entry_put;
    uint32_t last_entry_put;
    uint32_t last_exit_put;
    uint32_t first_entry_ahead;
    uint32_t last_exit_ahead;
    uint64_t total_ticks;
    uint64_t episode_count;
    uint64_t poll_count;
    uint64_t word_change_count;
    uint64_t put_change_count;
    uint64_t first_second;
    uint64_t last_second;
    int valid;
};

struct ActiveSemaphore {
    yz_rsx_semaphore_wait key;
    uint32_t table_index;
    uint32_t last_value;
    uint64_t start_qpc;
    int valid;
};

struct ActiveStopper {
    yz_rsx_stopper_wait key;
    uint32_t table_index;
    uint32_t last_word;
    uint32_t last_put;
    uint32_t last_ahead;
    uint64_t start_qpc;
    int valid;
};

struct Classifier {
    Bucket buckets[kBucketCount];
    Totals totals;
    SemaphoreAggregate semaphore[kSemaphoreAggregateCount];
    StopperAggregate stopper[kStopperAggregateCount];
    ActiveSemaphore active_semaphore;
    ActiveStopper active_stopper;
    uint64_t semaphore_overflow;
    uint64_t stopper_overflow;
    uint64_t qpc_frequency;
    uint64_t start_qpc;
    uint64_t phase_qpc;
    uint64_t last_completed_draws;
    uint32_t last_put;
    yz_rsx_wait_category phase;
    int phase_valid;
    int draw_total_valid;
    int put_valid;
    std::atomic<uint64_t> current_second;
    std::atomic<int> shutdown_started;
    HANDLE stop_event;
    HANDLE bucket_timer;
    HANDLE ticker_thread;
    int ticker_started;
#if defined(YZ_RSX_WAIT_CLASSIFIER_TEST)
    uint64_t test_now;
    uint64_t test_clock_reads;
    int test_clock;
#endif
};

Classifier g_classifier = {};

uint64_t mix32(uint64_t hash, uint32_t value)
{
    return (hash ^ value) * 1099511628211ull;
}

uint64_t semaphore_hash(const yz_rsx_semaphore_wait& key)
{
    uint64_t h = 1469598103934665603ull;
    h = mix32(h, key.context_dma);
    h = mix32(h, key.offset);
    h = mix32(h, key.address);
    return mix32(h, key.wanted);
}

uint64_t stopper_hash(const yz_rsx_stopper_wait& key)
{
    return mix32(mix32(1469598103934665603ull, key.get), key.address);
}

bool same_semaphore_key(const yz_rsx_semaphore_wait& a,
                        const yz_rsx_semaphore_wait& b)
{
    return a.context_dma == b.context_dma && a.offset == b.offset &&
        a.address == b.address && a.wanted == b.wanted;
}

bool same_stopper_key(const yz_rsx_stopper_wait& a,
                      const yz_rsx_stopper_wait& b)
{
    return a.get == b.get && a.address == b.address;
}

Bucket* bucket_for(uint64_t second)
{
    Bucket* b = &g_classifier.buckets[second % kBucketCount];
    if (!b->valid || b->second != second) {
        std::memset(b, 0, sizeof(*b));
        b->valid = 1;
        b->second = second;
    }
    return b;
}

uint64_t qpc_second(uint64_t qpc)
{
    if (!g_classifier.qpc_frequency || qpc <= g_classifier.start_qpc)
        return 0;
    return (qpc - g_classifier.start_qpc) / g_classifier.qpc_frequency;
}

uint64_t classifier_now(void)
{
#if defined(YZ_RSX_WAIT_CLASSIFIER_TEST)
    if (g_classifier.test_clock) {
        ++g_classifier.test_clock_reads;
        return g_classifier.test_now;
    }
#endif
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
}

void record_wall(yz_rsx_wait_category category, uint64_t begin, uint64_t end)
{
    if (category >= YZ_RSX_WAIT_CATEGORY_COUNT || end <= begin ||
        !g_classifier.qpc_frequency)
        return;
    g_classifier.totals.wall_ticks[category] += end - begin;
    while (begin < end) {
        const uint64_t second = qpc_second(begin);
        uint64_t boundary = g_classifier.start_qpc +
            (second + 1u) * g_classifier.qpc_frequency;
        if (boundary <= begin) boundary = begin + 1u;
        const uint64_t piece_end = boundary < end ? boundary : end;
        bucket_for(second)->wall_ticks[category] += piece_end - begin;
        begin = piece_end;
    }
}

SemaphoreAggregate* find_semaphore(const yz_rsx_semaphore_wait& key,
                                   bool create, uint32_t* index_out)
{
    const uint32_t mask = kSemaphoreAggregateCount - 1u;
    const uint32_t start = (uint32_t)semaphore_hash(key) & mask;
    for (uint32_t probe = 0; probe < kSemaphoreAggregateCount; ++probe) {
        const uint32_t index = (start + probe) & mask;
        SemaphoreAggregate* a = &g_classifier.semaphore[index];
        if (a->valid) {
            if (!same_semaphore_key(a->key, key)) continue;
            if (index_out) *index_out = index;
            return a;
        }
        if (!create) return nullptr;
        std::memset(a, 0, sizeof(*a));
        a->valid = 1;
        a->key = key;
        a->key.observed = 0;
        if (index_out) *index_out = index;
        return a;
    }
    if (create) ++g_classifier.semaphore_overflow;
    return nullptr;
}

StopperAggregate* find_stopper(const yz_rsx_stopper_wait& key,
                               bool create, uint32_t* index_out)
{
    const uint32_t mask = kStopperAggregateCount - 1u;
    const uint32_t start = (uint32_t)stopper_hash(key) & mask;
    for (uint32_t probe = 0; probe < kStopperAggregateCount; ++probe) {
        const uint32_t index = (start + probe) & mask;
        StopperAggregate* a = &g_classifier.stopper[index];
        if (a->valid) {
            if (!same_stopper_key(a->key, key)) continue;
            if (index_out) *index_out = index;
            return a;
        }
        if (!create) return nullptr;
        std::memset(a, 0, sizeof(*a));
        a->valid = 1;
        a->key = key;
        a->key.put = a->key.put_ahead = a->key.word = 0;
        if (index_out) *index_out = index;
        return a;
    }
    if (create) ++g_classifier.stopper_overflow;
    return nullptr;
}

void close_semaphore(uint64_t now)
{
    ActiveSemaphore& active = g_classifier.active_semaphore;
    if (!active.valid) return;
    if (active.table_index < kSemaphoreAggregateCount) {
        SemaphoreAggregate& a = g_classifier.semaphore[active.table_index];
        if (a.valid && now >= active.start_qpc) {
            a.total_ticks += now - active.start_qpc;
            a.last_exit_value = active.last_value;
            a.last_second = qpc_second(now);
        }
    }
    active.valid = 0;
}

void close_stopper(uint64_t now)
{
    ActiveStopper& active = g_classifier.active_stopper;
    if (!active.valid) return;
    if (active.table_index < kStopperAggregateCount) {
        StopperAggregate& a = g_classifier.stopper[active.table_index];
        if (a.valid && now >= active.start_qpc) {
            a.total_ticks += now - active.start_qpc;
            a.last_exit_word = active.last_word;
            a.last_exit_put = active.last_put;
            a.last_exit_ahead = active.last_ahead;
            a.last_second = qpc_second(now);
        }
    }
    active.valid = 0;
}

void transition_at(yz_rsx_wait_category category, uint64_t now)
{
    if (category >= YZ_RSX_WAIT_CATEGORY_COUNT)
        category = YZ_RSX_WAIT_BAD_FLOW;
    if (g_classifier.phase_valid && g_classifier.phase == category) return;
    if (g_classifier.phase_valid) {
        record_wall(g_classifier.phase, g_classifier.phase_qpc, now);
        ++g_classifier.totals.transitions[category];
        ++bucket_for(qpc_second(now))->transitions[category];
    }
    if (category != YZ_RSX_WAIT_SEMAPHORE) close_semaphore(now);
    if (category != YZ_RSX_WAIT_SELF_STOPPER) close_stopper(now);
    g_classifier.phase = category;
    g_classifier.phase_qpc = now;
    g_classifier.phase_valid = 1;
}

void start_semaphore(const yz_rsx_semaphore_wait& wait, uint64_t now)
{
    uint32_t index = 0;
    SemaphoreAggregate* a = find_semaphore(wait, true, &index);
    if (a) {
        if (!a->episode_count) {
            a->first_entry_value = wait.observed;
            a->first_second = qpc_second(now);
        }
        a->last_entry_value = a->last_exit_value = wait.observed;
        a->last_second = qpc_second(now);
        ++a->episode_count;
        ++a->poll_count;
    }
    ActiveSemaphore& active = g_classifier.active_semaphore;
    active.key = wait;
    active.key.observed = 0;
    active.table_index = a ? index : UINT32_MAX;
    active.last_value = wait.observed;
    active.start_qpc = now;
    active.valid = 1;
}

void update_semaphore(uint32_t observed, bool poll)
{
    ActiveSemaphore& active = g_classifier.active_semaphore;
    if (!active.valid) return;
    if (active.table_index >= kSemaphoreAggregateCount) {
        active.last_value = observed;
        return;
    }
    SemaphoreAggregate& a = g_classifier.semaphore[active.table_index];
    if (poll) ++a.poll_count;
    if (observed != active.last_value) {
        ++a.value_change_count;
        active.last_value = observed;
    }
    a.last_exit_value = observed;
}

void start_stopper(const yz_rsx_stopper_wait& wait, uint64_t now)
{
    uint32_t index = 0;
    StopperAggregate* a = find_stopper(wait, true, &index);
    if (a) {
        if (!a->episode_count) {
            a->first_entry_word = wait.word;
            a->first_entry_put = wait.put;
            a->first_entry_ahead = wait.put_ahead;
            a->first_second = qpc_second(now);
        }
        a->last_entry_word = a->last_exit_word = wait.word;
        a->last_entry_put = a->last_exit_put = wait.put;
        a->last_exit_ahead = wait.put_ahead;
        a->last_second = qpc_second(now);
        ++a->episode_count;
        ++a->poll_count;
    }
    ActiveStopper& active = g_classifier.active_stopper;
    active.key = wait;
    active.key.put = active.key.put_ahead = active.key.word = 0;
    active.table_index = a ? index : UINT32_MAX;
    active.last_word = wait.word;
    active.last_put = wait.put;
    active.last_ahead = wait.put_ahead;
    active.start_qpc = now;
    active.valid = 1;
}

void update_stopper(const yz_rsx_stopper_wait& wait, bool poll)
{
    ActiveStopper& active = g_classifier.active_stopper;
    if (!active.valid) return;
    if (active.table_index >= kStopperAggregateCount) {
        active.last_word = wait.word;
        active.last_put = wait.put;
        active.last_ahead = wait.put_ahead;
        return;
    }
    StopperAggregate& a = g_classifier.stopper[active.table_index];
    if (poll) ++a.poll_count;
    if (wait.word != active.last_word) {
        ++a.word_change_count;
        active.last_word = wait.word;
    }
    if (wait.put != active.last_put) {
        ++a.put_change_count;
        active.last_put = wait.put;
    }
    active.last_ahead = wait.put_ahead;
    a.last_exit_word = wait.word;
    a.last_exit_put = wait.put;
    a.last_exit_ahead = wait.put_ahead;
}

void record_current(yz_rsx_wait_category category,
                    uint32_t dispatched_methods,
                    uint64_t completed_draws,
                    uint32_t observed_put,
                    int have_put)
{
    if (category >= YZ_RSX_WAIT_CATEGORY_COUNT)
        category = YZ_RSX_WAIT_BAD_FLOW;
    const uint64_t second =
        g_classifier.current_second.load(std::memory_order_relaxed);
    Bucket* b = bucket_for(second);
    if (category == YZ_RSX_WAIT_ADVANCING) {
        ++b->progressing_steps;
        ++g_classifier.totals.progressing_steps;
    } else {
        ++b->polls[category];
        ++g_classifier.totals.polls[category];
    }
    b->dispatched_methods += dispatched_methods;
    g_classifier.totals.dispatched_methods += dispatched_methods;
    if (!g_classifier.draw_total_valid) {
        g_classifier.last_completed_draws = completed_draws;
        g_classifier.draw_total_valid = 1;
    } else {
        const uint64_t delta = completed_draws - g_classifier.last_completed_draws;
        b->completed_draws += delta;
        g_classifier.totals.completed_draws += delta;
        g_classifier.last_completed_draws = completed_draws;
    }
    if (have_put) {
        if (g_classifier.put_valid && observed_put != g_classifier.last_put) {
            ++b->observed_put_changes;
            ++g_classifier.totals.observed_put_changes;
        }
        g_classifier.last_put = observed_put;
        g_classifier.put_valid = 1;
    }
}

DWORD WINAPI ticker_main(LPVOID)
{
    HANDLE waits[2] = {g_classifier.stop_event, g_classifier.bucket_timer};
    for (;;) {
        const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) return 0;
        if (result != WAIT_OBJECT_0 + 1u) return 1;
        g_classifier.current_second.fetch_add(1u, std::memory_order_release);
    }
}

uint64_t ticks_to_microseconds(uint64_t ticks)
{
    if (!g_classifier.qpc_frequency) return 0;
    const uint64_t whole = ticks / g_classifier.qpc_frequency;
    const uint64_t remainder = ticks % g_classifier.qpc_frequency;
    return whole * 1000000u +
        (remainder * 1000000u) / g_classifier.qpc_frequency;
}

void print_vector_us(const uint64_t values[YZ_RSX_WAIT_CATEGORY_COUNT])
{
    for (uint32_t i = 0; i < YZ_RSX_WAIT_CATEGORY_COUNT; ++i) {
        if (i) std::fputc('/', stderr);
        std::fprintf(stderr, "%llu",
                     (unsigned long long)ticks_to_microseconds(values[i]));
    }
}

void print_vector_u64(const uint64_t values[YZ_RSX_WAIT_CATEGORY_COUNT])
{
    for (uint32_t i = 0; i < YZ_RSX_WAIT_CATEGORY_COUNT; ++i) {
        if (i) std::fputc('/', stderr);
        std::fprintf(stderr, "%llu", (unsigned long long)values[i]);
    }
}

uint64_t sum_vector(const uint64_t values[YZ_RSX_WAIT_CATEGORY_COUNT])
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < YZ_RSX_WAIT_CATEGORY_COUNT; ++i)
        total += values[i];
    return total;
}

void reset_data(uint64_t frequency, uint64_t start)
{
    std::memset(g_classifier.buckets, 0, sizeof(g_classifier.buckets));
    std::memset(&g_classifier.totals, 0, sizeof(g_classifier.totals));
    std::memset(g_classifier.semaphore, 0, sizeof(g_classifier.semaphore));
    std::memset(g_classifier.stopper, 0, sizeof(g_classifier.stopper));
    std::memset(&g_classifier.active_semaphore, 0,
                sizeof(g_classifier.active_semaphore));
    std::memset(&g_classifier.active_stopper, 0,
                sizeof(g_classifier.active_stopper));
    g_classifier.semaphore_overflow = 0;
    g_classifier.stopper_overflow = 0;
    g_classifier.qpc_frequency = frequency;
    g_classifier.start_qpc = start;
    g_classifier.phase_qpc = start;
    g_classifier.last_completed_draws = 0;
    g_classifier.last_put = 0;
    g_classifier.phase = YZ_RSX_WAIT_ADVANCING;
    g_classifier.phase_valid = 0;
    g_classifier.draw_total_valid = 0;
    g_classifier.put_valid = 0;
    g_classifier.current_second.store(0, std::memory_order_relaxed);
    g_classifier.shutdown_started.store(0, std::memory_order_relaxed);
}

void finalize_at(uint64_t now)
{
    if (g_classifier.phase_valid)
        record_wall(g_classifier.phase, g_classifier.phase_qpc, now);
    close_semaphore(now);
    close_stopper(now);
}

} // namespace

extern "C" int g_yz_rsx_wait_classifier_enabled = 0;

extern "C" const char* yz_rsx_wait_category_name(
    yz_rsx_wait_category category)
{
    static const char* names[YZ_RSX_WAIT_CATEGORY_COUNT] = {
        "ADVANCING", "WAIT_EMPTY", "WAIT_PARTIAL_PACKET",
        "WAIT_SELF_STOPPER", "WAIT_SEMAPHORE", "WAIT_UNFINALIZED_HOLE",
        "WAIT_BAD_FLOW", "WAIT_NO_CONTEXT"
    };
    return category < YZ_RSX_WAIT_CATEGORY_COUNT ? names[category]
                                                  : "WAIT_BAD_FLOW";
}

extern "C" int yz_rsx_wait_classifier_init(void)
{
    static LONG initialized = 0;
    if (InterlockedCompareExchange(&initialized, 1, 0) != 0)
        return g_yz_rsx_wait_classifier_enabled;
    const char* value = std::getenv("YZ_RSX_WAIT_CLASSIFIER");
    if (!value || std::strcmp(value, "1") != 0) return 0;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&start) || frequency.QuadPart <= 0)
        return 0;
    reset_data((uint64_t)frequency.QuadPart, (uint64_t)start.QuadPart);
    g_classifier.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_classifier.bucket_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (g_classifier.stop_event && g_classifier.bucket_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -10000000ll;
        if (SetWaitableTimer(g_classifier.bucket_timer, &due,
                             (LONG)kBucketMilliseconds,
                             nullptr, nullptr, FALSE)) {
            g_classifier.ticker_thread =
                CreateThread(nullptr, 0, ticker_main, nullptr, 0, nullptr);
            g_classifier.ticker_started = g_classifier.ticker_thread != nullptr;
        }
    }
    g_yz_rsx_wait_classifier_enabled = 1;
    return 1;
}

extern "C" void yz_rsx_wait_classifier_set_completed_draw_baseline(
    uint64_t completed_draws)
{
    if (!g_yz_rsx_wait_classifier_enabled ||
        g_classifier.shutdown_started.load(std::memory_order_acquire)) return;
    g_classifier.last_completed_draws = completed_draws;
    g_classifier.draw_total_valid = 1;
}

extern "C" void yz_rsx_wait_classifier_transition(
    yz_rsx_wait_category category)
{
    if (!g_yz_rsx_wait_classifier_enabled ||
        g_classifier.shutdown_started.load(std::memory_order_acquire)) return;
    if (category < YZ_RSX_WAIT_CATEGORY_COUNT && g_classifier.phase_valid &&
        g_classifier.phase == category) return;
    transition_at(category, classifier_now());
}

extern "C" void yz_rsx_wait_classifier_semaphore_attempt(
    const yz_rsx_semaphore_wait* wait, int stalled)
{
    if (!g_yz_rsx_wait_classifier_enabled || !wait ||
        g_classifier.shutdown_started.load(std::memory_order_acquire)) return;
    ActiveSemaphore& active = g_classifier.active_semaphore;
    if (!stalled) {
        if (active.valid && same_semaphore_key(active.key, *wait))
            update_semaphore(wait->observed, false);
        return;
    }
    if (active.valid && same_semaphore_key(active.key, *wait)) {
        update_semaphore(wait->observed, true);
        return;
    }
    const uint64_t now = classifier_now();
    if (active.valid) close_semaphore(now);
    transition_at(YZ_RSX_WAIT_SEMAPHORE, now);
    start_semaphore(*wait, now);
}

extern "C" void yz_rsx_wait_classifier_stopper_observe(
    const yz_rsx_stopper_wait* wait, int waiting)
{
    if (!g_yz_rsx_wait_classifier_enabled || !wait ||
        g_classifier.shutdown_started.load(std::memory_order_acquire)) return;
    ActiveStopper& active = g_classifier.active_stopper;
    if (!waiting) {
        if (active.valid && same_stopper_key(active.key, *wait))
            update_stopper(*wait, false);
        return;
    }
    if (active.valid && same_stopper_key(active.key, *wait)) {
        update_stopper(*wait, true);
        return;
    }
    const uint64_t now = classifier_now();
    if (active.valid) close_stopper(now);
    transition_at(YZ_RSX_WAIT_SELF_STOPPER, now);
    start_stopper(*wait, now);
}

extern "C" void yz_rsx_wait_classifier_record(
    yz_rsx_wait_category category, uint32_t dispatched_methods,
    uint64_t completed_draws, uint32_t observed_put, int have_put)
{
    if (!g_yz_rsx_wait_classifier_enabled ||
        g_classifier.shutdown_started.load(std::memory_order_acquire)) return;
    yz_rsx_wait_classifier_transition(category);
    record_current(category, dispatched_methods, completed_draws,
                   observed_put, have_put);
}

extern "C" void yz_rsx_wait_classifier_shutdown(void)
{
    if (!g_yz_rsx_wait_classifier_enabled ||
        g_classifier.shutdown_started.exchange(1, std::memory_order_acq_rel))
        return;
    if (g_classifier.stop_event) SetEvent(g_classifier.stop_event);
    if (g_classifier.ticker_thread)
        WaitForSingleObject(g_classifier.ticker_thread, 2000);
    const uint64_t end_qpc = classifier_now();
    finalize_at(end_qpc);

    const uint64_t final_second = qpc_second(end_qpc);
    const uint64_t first_second = final_second >= kBucketCount
        ? final_second - kBucketCount + 1u : 0u;
    std::fprintf(stderr,
        "[rsx-wait-aggregate] version=2 flag=YZ_RSX_WAIT_CLASSIFIER "
        "bucket_ms=%lu capacity=%llu first=%llu last=%llu dropped_prefix=%llu "
        "ticker=%s category_order=ADVANCING/WAIT_EMPTY/WAIT_PARTIAL_PACKET/"
        "WAIT_SELF_STOPPER/WAIT_SEMAPHORE/WAIT_UNFINALIZED_HOLE/WAIT_BAD_FLOW/"
        "WAIT_NO_CONTEXT\n",
        (unsigned long)kBucketMilliseconds,
        (unsigned long long)kBucketCount,
        (unsigned long long)first_second,
        (unsigned long long)final_second,
        (unsigned long long)first_second,
        g_classifier.ticker_started ? "periodic" : "unavailable");

    for (uint64_t second = first_second; second <= final_second; ++second) {
        Bucket empty = {};
        empty.second = second;
        const Bucket* b = &g_classifier.buckets[second % kBucketCount];
        if (!b->valid || b->second != second) b = &empty;
        std::fprintf(stderr,
                     "[rsx-wait-bucket] second=%llu duration_us=%llu wall_us=",
                     (unsigned long long)second,
                     (unsigned long long)ticks_to_microseconds(
                         sum_vector(b->wall_ticks)));
        print_vector_us(b->wall_ticks);
        std::fprintf(stderr, " transition_count=%llu transitions=",
                     (unsigned long long)sum_vector(b->transitions));
        print_vector_u64(b->transitions);
        std::fprintf(stderr, " poll_count=%llu polls=",
                     (unsigned long long)sum_vector(b->polls));
        print_vector_u64(b->polls);
        std::fprintf(stderr,
            " steps=%llu methods=%llu draws=%llu put_changes=%llu\n",
            (unsigned long long)b->progressing_steps,
            (unsigned long long)b->dispatched_methods,
            (unsigned long long)b->completed_draws,
            (unsigned long long)b->observed_put_changes);
        if (second == UINT64_MAX) break;
    }

    std::fprintf(stderr, "[rsx-wait-total] duration_us=%llu wall_us=",
                 (unsigned long long)ticks_to_microseconds(
                     sum_vector(g_classifier.totals.wall_ticks)));
    print_vector_us(g_classifier.totals.wall_ticks);
    std::fprintf(stderr, " transition_count=%llu transitions=",
                 (unsigned long long)sum_vector(g_classifier.totals.transitions));
    print_vector_u64(g_classifier.totals.transitions);
    std::fprintf(stderr, " poll_count=%llu polls=",
                 (unsigned long long)sum_vector(g_classifier.totals.polls));
    print_vector_u64(g_classifier.totals.polls);
    std::fprintf(stderr,
        " steps=%llu methods=%llu draws=%llu put_changes=%llu "
        "semaphore_overflow=%llu stopper_overflow=%llu\n",
        (unsigned long long)g_classifier.totals.progressing_steps,
        (unsigned long long)g_classifier.totals.dispatched_methods,
        (unsigned long long)g_classifier.totals.completed_draws,
        (unsigned long long)g_classifier.totals.observed_put_changes,
        (unsigned long long)g_classifier.semaphore_overflow,
        (unsigned long long)g_classifier.stopper_overflow);

    for (uint32_t i = 0; i < kSemaphoreAggregateCount; ++i) {
        const SemaphoreAggregate& a = g_classifier.semaphore[i];
        if (!a.valid) continue;
        std::fprintf(stderr,
            "[rsx-wait-semaphore] dma=0x%08X offset=0x%08X "
            "address=0x%08X wanted=0x%08X entry_first=0x%08X "
            "entry_last=0x%08X exit_last=0x%08X duration_us=%llu "
            "episodes=%llu polls=%llu value_changes=%llu changed=%u "
            "first_second=%llu last_second=%llu\n",
            a.key.context_dma, a.key.offset, a.key.address, a.key.wanted,
            a.first_entry_value, a.last_entry_value, a.last_exit_value,
            (unsigned long long)ticks_to_microseconds(a.total_ticks),
            (unsigned long long)a.episode_count,
            (unsigned long long)a.poll_count,
            (unsigned long long)a.value_change_count,
            a.value_change_count ? 1u : 0u,
            (unsigned long long)a.first_second,
            (unsigned long long)a.last_second);
    }
    for (uint32_t i = 0; i < kStopperAggregateCount; ++i) {
        const StopperAggregate& a = g_classifier.stopper[i];
        if (!a.valid) continue;
        std::fprintf(stderr,
            "[rsx-wait-stopper] get=0x%08X address=0x%08X "
            "entry_word_first=0x%08X entry_word_last=0x%08X "
            "exit_word_last=0x%08X entry_put_first=0x%08X "
            "entry_put_last=0x%08X exit_put_last=0x%08X "
            "entry_ahead_first=0x%08X exit_ahead_last=0x%08X "
            "duration_us=%llu episodes=%llu polls=%llu word_changes=%llu "
            "put_changes=%llu word_changed=%u put_changed=%u "
            "first_second=%llu last_second=%llu\n",
            a.key.get, a.key.address, a.first_entry_word, a.last_entry_word,
            a.last_exit_word, a.first_entry_put, a.last_entry_put,
            a.last_exit_put, a.first_entry_ahead, a.last_exit_ahead,
            (unsigned long long)ticks_to_microseconds(a.total_ticks),
            (unsigned long long)a.episode_count,
            (unsigned long long)a.poll_count,
            (unsigned long long)a.word_change_count,
            (unsigned long long)a.put_change_count,
            a.word_change_count ? 1u : 0u,
            a.put_change_count ? 1u : 0u,
            (unsigned long long)a.first_second,
            (unsigned long long)a.last_second);
    }
    std::fflush(stderr);

    if (g_classifier.bucket_timer) {
        CancelWaitableTimer(g_classifier.bucket_timer);
        CloseHandle(g_classifier.bucket_timer);
        g_classifier.bucket_timer = nullptr;
    }
    if (g_classifier.ticker_thread) {
        CloseHandle(g_classifier.ticker_thread);
        g_classifier.ticker_thread = nullptr;
    }
    if (g_classifier.stop_event) {
        CloseHandle(g_classifier.stop_event);
        g_classifier.stop_event = nullptr;
    }
}

#if defined(YZ_RSX_WAIT_CLASSIFIER_TEST)

extern "C" void yz_rsx_wait_classifier_test_reset(
    uint64_t qpc_frequency, uint64_t start_qpc, int enabled)
{
    reset_data(qpc_frequency, start_qpc);
    g_classifier.test_now = start_qpc;
    g_classifier.test_clock_reads = 0;
    g_classifier.test_clock = 1;
    g_yz_rsx_wait_classifier_enabled = enabled ? 1 : 0;
}

extern "C" void yz_rsx_wait_classifier_test_set_clock(uint64_t now_qpc)
{
    g_classifier.test_now = now_qpc;
}

extern "C" void yz_rsx_wait_classifier_test_set_second(uint64_t second)
{
    g_classifier.current_second.store(second, std::memory_order_relaxed);
}

extern "C" uint64_t yz_rsx_wait_classifier_test_clock_reads(void)
{
    return g_classifier.test_clock_reads;
}

extern "C" void yz_rsx_wait_classifier_test_transition(
    yz_rsx_wait_category category)
{
    yz_rsx_wait_classifier_transition(category);
}

extern "C" void yz_rsx_wait_classifier_test_record(
    yz_rsx_wait_category category, uint32_t dispatched_methods,
    uint64_t completed_draws, uint32_t observed_put, int have_put)
{
    yz_rsx_wait_classifier_record(category, dispatched_methods,
                                  completed_draws, observed_put, have_put);
}

extern "C" void yz_rsx_wait_classifier_test_shutdown(void)
{
    if (!g_yz_rsx_wait_classifier_enabled ||
        g_classifier.shutdown_started.exchange(1, std::memory_order_acq_rel))
        return;
    finalize_at(classifier_now());
}

extern "C" int yz_rsx_wait_classifier_test_bucket(
    uint64_t second, yz_rsx_wait_test_bucket* out)
{
    if (!out) return 0;
    const Bucket* b = &g_classifier.buckets[second % kBucketCount];
    if (!b->valid || b->second != second) return 0;
    std::memcpy(out, b, sizeof(*out));
    return 1;
}

extern "C" int yz_rsx_wait_classifier_test_find_semaphore(
    const yz_rsx_semaphore_wait* key,
    yz_rsx_wait_test_semaphore_aggregate* out)
{
    if (!key || !out) return 0;
    SemaphoreAggregate* a = find_semaphore(*key, false, nullptr);
    if (!a) return 0;
    out->key = a->key;
    out->first_entry_value = a->first_entry_value;
    out->last_entry_value = a->last_entry_value;
    out->last_exit_value = a->last_exit_value;
    out->total_ticks = a->total_ticks;
    out->episode_count = a->episode_count;
    out->poll_count = a->poll_count;
    out->value_change_count = a->value_change_count;
    out->first_second = a->first_second;
    out->last_second = a->last_second;
    return 1;
}

extern "C" int yz_rsx_wait_classifier_test_find_stopper(
    const yz_rsx_stopper_wait* key,
    yz_rsx_wait_test_stopper_aggregate* out)
{
    if (!key || !out) return 0;
    StopperAggregate* a = find_stopper(*key, false, nullptr);
    if (!a) return 0;
    out->key = a->key;
    out->first_entry_word = a->first_entry_word;
    out->last_entry_word = a->last_entry_word;
    out->last_exit_word = a->last_exit_word;
    out->first_entry_put = a->first_entry_put;
    out->last_entry_put = a->last_entry_put;
    out->last_exit_put = a->last_exit_put;
    out->first_entry_ahead = a->first_entry_ahead;
    out->last_exit_ahead = a->last_exit_ahead;
    out->total_ticks = a->total_ticks;
    out->episode_count = a->episode_count;
    out->poll_count = a->poll_count;
    out->word_change_count = a->word_change_count;
    out->put_change_count = a->put_change_count;
    out->first_second = a->first_second;
    out->last_second = a->last_second;
    return 1;
}

extern "C" uint64_t yz_rsx_wait_classifier_test_semaphore_overflow(void)
{
    return g_classifier.semaphore_overflow;
}

extern "C" uint64_t yz_rsx_wait_classifier_test_stopper_overflow(void)
{
    return g_classifier.stopper_overflow;
}

static_assert(sizeof(yz_rsx_wait_test_bucket) == sizeof(Bucket),
              "test bucket ABI must match production bucket");

#endif
