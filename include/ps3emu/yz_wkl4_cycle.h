#ifndef PS3EMU_YZ_WKL4_CYCLE_H
#define PS3EMU_YZ_WKL4_CYCLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Focused image-4 cycle attribution.  The hooks exist only in a build made
 * with YZ_WKL4_CYCLE_DIAGNOSTIC=ON, and collection is additionally default
 * off unless YZ_WKL4_CYCLE=1.  Enabled collection uses thread-local state
 * plus fixed atomic aggregates; shutdown is the only I/O.
 */
typedef enum yz_wkl4_cycle_tag {
    YZ_WKL4_CYCLE_NONE = 0,
    YZ_WKL4_CYCLE_7E50_SETUP,
    YZ_WKL4_CYCLE_7E50_LOOP,
    YZ_WKL4_CYCLE_7E50_SCATTER,
    YZ_WKL4_CYCLE_7E50_SHUFFLE,
    YZ_WKL4_CYCLE_7E50_COMMIT,
    YZ_WKL4_CYCLE_7E50_TAIL,
    YZ_WKL4_CYCLE_8230_SETUP,
    YZ_WKL4_CYCLE_8230_LOOP,
    YZ_WKL4_CYCLE_8230_COMPARE,
    YZ_WKL4_CYCLE_8230_STORE,
    YZ_WKL4_CYCLE_8230_TAIL,
    YZ_WKL4_CYCLE_8680_SETUP,
    YZ_WKL4_CYCLE_8680_LOOP,
    YZ_WKL4_CYCLE_8680_TAIL,
    YZ_WKL4_CYCLE_9808_SETUP,
    YZ_WKL4_CYCLE_9808_LOOP,
    YZ_WKL4_CYCLE_9808_TAIL,
    YZ_WKL4_CYCLE_TAG_COUNT
} yz_wkl4_cycle_tag;

extern int g_yz_wkl4_cycle_enabled;

int yz_wkl4_cycle_init(void);
void yz_wkl4_cycle_mark(yz_wkl4_cycle_tag tag);
void yz_wkl4_cycle_leave(void);
void yz_wkl4_cycle_shutdown(void);
const char* yz_wkl4_cycle_tag_name(yz_wkl4_cycle_tag tag);

#if defined(YZ_WKL4_CYCLE_TEST)
void yz_wkl4_cycle_test_reset(int enabled);
void yz_wkl4_cycle_test_set_clock(uint64_t cycles);
uint64_t yz_wkl4_cycle_test_clock_reads(void);
uint64_t yz_wkl4_cycle_test_cycles(yz_wkl4_cycle_tag tag);
uint64_t yz_wkl4_cycle_test_entries(yz_wkl4_cycle_tag tag);
#endif

#ifdef __cplusplus
}
#endif

#if defined(YZ_WKL4_CYCLE_DIAGNOSTIC)
#define YZ_WKL4_CYCLE_MARK(tag) do {                              \
        if (g_yz_wkl4_cycle_enabled)                              \
            yz_wkl4_cycle_mark((tag));                            \
    } while (0)
#define YZ_WKL4_CYCLE_LEAVE() do {                                \
        if (g_yz_wkl4_cycle_enabled)                              \
            yz_wkl4_cycle_leave();                                \
    } while (0)
#else
#define YZ_WKL4_CYCLE_MARK(tag) ((void)0)
#define YZ_WKL4_CYCLE_LEAVE() ((void)0)
#endif

#endif
