/*
 * ps3recomp - high-resolution deadlines for short LV2 waits
 *
 * Kept separate from sys_timer.c so synchronization primitives and their
 * standalone stress suite can share the real deadline implementation without
 * pulling in the complete timer/event subsystem.
 */

#include "sys_timer.h"

#ifdef _WIN32

static LARGE_INTEGER s_deadline_qpc_freq;
static int s_deadline_qpc_init;

static void ensure_deadline_qpc_init(void)
{
    if (!s_deadline_qpc_init) {
        QueryPerformanceFrequency(&s_deadline_qpc_freq);
        s_deadline_qpc_init = 1;
    }
}

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    ensure_deadline_qpc_init();
    QueryPerformanceCounter(&now);
    return now.QuadPart +
        (int64_t)((usec * (uint64_t)s_deadline_qpc_freq.QuadPart) / 1000000ULL);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}

#endif
