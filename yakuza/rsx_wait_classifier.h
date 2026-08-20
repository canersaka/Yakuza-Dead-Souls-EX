#ifndef YAKUZA_RSX_WAIT_CLASSIFIER_H
#define YAKUZA_RSX_WAIT_CLASSIFIER_H

#include <stdint.h>

enum yz_rsx_wait_category : uint32_t {
    YZ_RSX_WAIT_ADVANCING = 0,
    YZ_RSX_WAIT_EMPTY,
    YZ_RSX_WAIT_PARTIAL_PACKET,
    YZ_RSX_WAIT_SELF_STOPPER,
    YZ_RSX_WAIT_SEMAPHORE,
    YZ_RSX_WAIT_UNFINALIZED_HOLE,
    YZ_RSX_WAIT_BAD_FLOW,
    YZ_RSX_WAIT_NO_CONTEXT,
    YZ_RSX_WAIT_CATEGORY_COUNT
};

struct yz_rsx_semaphore_wait {
    uint32_t context_dma;
    uint32_t offset;
    uint32_t address;
    uint32_t wanted;
    uint32_t observed;
};

struct yz_rsx_stopper_wait {
    uint32_t get;
    uint32_t address;
    uint32_t put;
    uint32_t put_ahead;
    uint32_t word;
};

extern "C" {

/* Immutable after initialization. The production FIFO selects a fully
 * uninstrumented template specialization when this is zero. */
extern int g_yz_rsx_wait_classifier_enabled;

/* Reads YZ_RSX_WAIT_CLASSIFIER exactly once. Only the exact value "1"
 * enables collection. */
int yz_rsx_wait_classifier_init(void);

void yz_rsx_wait_classifier_set_completed_draw_baseline(uint64_t completed_draws);

/* Enter a genuine phase. The clock is queried only if the phase differs. */
void yz_rsx_wait_classifier_transition(yz_rsx_wait_category category);

/* Repeated stalls of one key update counters without querying the clock. */
void yz_rsx_wait_classifier_semaphore_attempt(
    const yz_rsx_semaphore_wait* wait, int stalled);

/* waiting=0 only refreshes an already-active stopper episode's exit state. */
void yz_rsx_wait_classifier_stopper_observe(
    const yz_rsx_stopper_wait* wait, int waiting);

void yz_rsx_wait_classifier_record(
    yz_rsx_wait_category category,
    uint32_t dispatched_methods,
    uint64_t completed_draws,
    uint32_t observed_put,
    int have_put);

void yz_rsx_wait_classifier_shutdown(void);
void yz_rsx_wait_classifier_shutdown_serialized(void);
const char* yz_rsx_wait_category_name(yz_rsx_wait_category category);

}

#if defined(YZ_RSX_WAIT_CLASSIFIER_TEST)

#define YZ_RSX_WAIT_TEST_BUCKET_CAP 4096u

struct yz_rsx_wait_test_bucket {
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

struct yz_rsx_wait_test_semaphore_aggregate {
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
};

struct yz_rsx_wait_test_stopper_aggregate {
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
};

extern "C" {
void yz_rsx_wait_classifier_test_reset(
    uint64_t qpc_frequency, uint64_t start_qpc, int enabled);
void yz_rsx_wait_classifier_test_set_clock(uint64_t now_qpc);
void yz_rsx_wait_classifier_test_set_second(uint64_t second);
uint64_t yz_rsx_wait_classifier_test_clock_reads(void);
void yz_rsx_wait_classifier_test_transition(yz_rsx_wait_category category);
void yz_rsx_wait_classifier_test_record(
    yz_rsx_wait_category category,
    uint32_t dispatched_methods,
    uint64_t completed_draws,
    uint32_t observed_put,
    int have_put);
void yz_rsx_wait_classifier_test_shutdown(void);
int yz_rsx_wait_classifier_test_bucket(
    uint64_t second, yz_rsx_wait_test_bucket* out);
int yz_rsx_wait_classifier_test_find_semaphore(
    const yz_rsx_semaphore_wait* key,
    yz_rsx_wait_test_semaphore_aggregate* out);
int yz_rsx_wait_classifier_test_find_stopper(
    const yz_rsx_stopper_wait* key,
    yz_rsx_wait_test_stopper_aggregate* out);
uint64_t yz_rsx_wait_classifier_test_semaphore_overflow(void);
uint64_t yz_rsx_wait_classifier_test_stopper_overflow(void);
}

#endif

#endif
