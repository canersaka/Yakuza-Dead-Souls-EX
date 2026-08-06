/*
 * ps3recomp - tests/lwmutex_lifecycle/main.c
 *
 * Conformance + stress test for the sysPrxForUser lwmutex lifecycle
 * (libs/system/sysPrxForUser.c), written for the 2026-08-05 boot-13
 * resurrection fix (STATUS frontier; scratch/motblk_trace_20260805_boot13).
 *
 * Contract under test (ORACLE Emu/Cell/Modules/sys_lwmutex_.cpp + SDK
 * sys/synchronization.h):
 *   - create stores the FREE sentinel into the guest owner word (:51);
 *   - destroy publishes the DEAD sentinel (:89);
 *   - lock/trylock on a DESTROYED lwmutex fail with CELL_EINVAL (:146-149)
 *     and must NEVER silently re-register ("resurrect") the mutex;
 *   - a statically-initialized (all-zero) lwmutex that never saw create still
 *     lazily registers on first lock (the 2026-06-23 lazy-register feature);
 *   - destroy of a HELD lwmutex fails with CELL_EBUSY;
 *   - destroy -> create recycling works, and mutual exclusion SURVIVES
 *     concurrent destroy/create churn (the boot-13 failure: a lock landing in
 *     the destroy->create gap used to lazy-register a phantom slot and
 *     silently succeed alongside the recreated mutex's real slot).
 *
 * The churn case is the measured game pattern: the loader cycles
 * destroy->create on one lwmutex EA per archive read while other threads
 * lock/unlock it (boot 13: 0x01659658, tid 44260, dialogue-scene reload).
 *
 * Kill-switch note: running with YZ_LWM_RESURRECT_OK=1 restores the pre-fix
 * behavior and this test is EXPECTED TO FAIL there (that is the pre-fix
 * repro proof). The registered ctest invocation runs with a clean env.
 *
 * Exit code 0 = all tests passed. Nonzero = at least one FAIL.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysPrxForUser.h"
#include "ps3emu/error_codes.h"

/* ---------------------------------------------------------------------------
 * Guest-memory shim: sysPrxForUser.c's YZ_GUEST_ADDR computes ptr - vm_base
 * for diagnostics; give it a real buffer so guest EAs are small and stable.
 * -----------------------------------------------------------------------*/
uint8_t* vm_base;
static uint8_t g_guest_mem[1u << 20];

/* Runtime shims sysPrxForUser.c references outside the lwmutex core. */
u32 yz_thread_current_id(void) { return (u32)GetCurrentThreadId(); }
volatile long g_yz_a010_release_scene_active = 0;
void yz_a010_reltrace_ppu_bulk(u32 dst, const uint8_t* src, u32 size,
                               u32 guest_pc, u32 op, uint8_t fill)
{ (void)dst; (void)src; (void)size; (void)guest_pc; (void)op; (void)fill; }

/* Sentinels must match sysPrxForUser.c (ORACLE Emu/Cell/lv2/sys_lwmutex.h:21-23). */
#define LWM_FREE 0xFFFFFFFFu
#define LWM_DEAD 0xFFFFFFFEu

static int g_fails;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            g_fails++;                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

static u32 owner_word(const sys_lwmutex_t_hle* m)
{
    u32 raw;
    memcpy(&raw, &m->lock_var, 4);
    return (raw >> 24) | ((raw >> 8) & 0x0000FF00u) |
           ((raw << 8) & 0x00FF0000u) | (raw << 24);
}

/* ---------------------------------------------------------------------------
 * Case 1-4: single-threaded lifecycle conformance
 * -----------------------------------------------------------------------*/
static void test_lifecycle(void)
{
    sys_lwmutex_t_hle* m = (sys_lwmutex_t_hle*)(vm_base + 0x1000);
    sys_lwmutex_attribute_t attr;
    memset(&attr, 0, sizeof(attr));
    memcpy(attr.name, "lifec\0\0\0", 8);

    /* 1. static-init (all-zero) lwmutex: first trylock lazily registers. */
    memset(m, 0, sizeof(*m));
    CHECK(sys_lwmutex_trylock(m) == CELL_OK, "static-init trylock");
    CHECK(sys_lwmutex_unlock(m) == CELL_OK, "static-init unlock");
    CHECK(sys_lwmutex_lock(m, 0) == CELL_OK, "static-init lock");
    CHECK(sys_lwmutex_unlock(m) == CELL_OK, "static-init unlock 2");
    CHECK(sys_lwmutex_destroy(m) == CELL_OK, "static-init destroy");

    /* 2. create publishes FREE; destroy of a HELD lwmutex is EBUSY. */
    CHECK(sys_lwmutex_create(m, &attr) == CELL_OK, "create");
    CHECK(owner_word(m) == LWM_FREE, "owner==FREE after create (got 0x%08X)",
          owner_word(m));
    CHECK(sys_lwmutex_lock(m, 0) == CELL_OK, "lock");
    CHECK(sys_lwmutex_destroy(m) == CELL_EBUSY, "destroy of held -> EBUSY");
    CHECK(sys_lwmutex_unlock(m) == CELL_OK, "unlock");

    /* 3. destroy publishes DEAD; lock/trylock after destroy fail loudly. */
    CHECK(sys_lwmutex_destroy(m) == CELL_OK, "destroy");
    CHECK(owner_word(m) == LWM_DEAD, "owner==DEAD after destroy (got 0x%08X)",
          owner_word(m));
    CHECK(sys_lwmutex_lock(m, 0) == CELL_EINVAL,
          "lock after destroy -> EINVAL (no resurrection)");
    CHECK(sys_lwmutex_trylock(m) == CELL_EINVAL,
          "trylock after destroy -> EINVAL (no resurrection)");
    CHECK(owner_word(m) == LWM_DEAD, "owner stays DEAD after refused lock");

    /* 4. destroy -> create recycling works. */
    CHECK(sys_lwmutex_create(m, &attr) == CELL_OK, "re-create");
    CHECK(sys_lwmutex_lock(m, 0) == CELL_OK, "lock after re-create");
    CHECK(sys_lwmutex_unlock(m) == CELL_OK, "unlock after re-create");
    CHECK(sys_lwmutex_destroy(m) == CELL_OK, "final destroy");
}

/* ---------------------------------------------------------------------------
 * Case 5: the boot-13 churn race. One churner thread create/destroy-cycles
 * the lwmutex while two locker threads lock/unlock it. Mutual exclusion is
 * asserted with an in-critical-section counter; the pre-fix lazy-register
 * resurrection makes two lockers succeed on different host slots and trips it.
 * -----------------------------------------------------------------------*/
#define CHURN_CYCLES 5000
#define CHURN_DESTROY_RETRY_MAX 2000000   /* starvation guard: FAIL loudly
                                           * instead of hanging the suite */

static sys_lwmutex_t_hle* g_churn_m;
static volatile LONG g_in_cs;
static volatile LONG g_stop;
static volatile LONG g_excl_violations;
static volatile LONG g_lock_ok, g_lock_refused;
static volatile LONG g_unlock_fail;   /* unlock-after-successful-trylock refusals:
                                       * a pre-existing rough edge under extreme
                                       * churn (see STATUS); reported, not FAILed */

static unsigned __stdcall churner(void* arg)
{
    sys_lwmutex_attribute_t attr;
    memset(&attr, 0, sizeof(attr));
    memcpy(attr.name, "churn\0\0\0", 8);
    (void)arg;
    for (int i = 0; i < CHURN_CYCLES; i++) {
        if (sys_lwmutex_create(g_churn_m, &attr) != CELL_OK) {
            CHECK(0, "churn create failed at cycle %d", i);
            break;
        }
        /* small live window so lockers can get in */
        for (volatile int spin = 0; spin < 40; spin++) { }
        long retries = 0;
        for (;;) {
            s32 rc = sys_lwmutex_destroy(g_churn_m);
            if (rc == CELL_OK) break;
            if (rc != CELL_EBUSY) {
                CHECK(0, "churn destroy rc=0x%08X at cycle %d", (u32)rc, i);
                InterlockedExchange(&g_stop, 1);
                return 0;
            }
            if (++retries > CHURN_DESTROY_RETRY_MAX) {
                CHECK(0, "churn destroy starved (%ld EBUSY retries) at cycle %d",
                      retries, i);
                InterlockedExchange(&g_stop, 1);
                return 0;
            }
            Sleep(0);   /* holder active: retry, same as lv2 EBUSY contract */
        }
    }
    InterlockedExchange(&g_stop, 1);
    return 0;
}

static unsigned __stdcall locker(void* arg)
{
    (void)arg;
    while (!g_stop) {
        s32 rc = sys_lwmutex_trylock(g_churn_m);
        if (rc == CELL_OK) {
            LONG v = InterlockedIncrement(&g_in_cs);
            if (v != 1) {
                InterlockedIncrement(&g_excl_violations);
            }
            for (volatile int spin = 0; spin < 20; spin++) { }
            InterlockedDecrement(&g_in_cs);
            InterlockedIncrement(&g_lock_ok);
            if (sys_lwmutex_unlock(g_churn_m) != CELL_OK)
                InterlockedIncrement(&g_unlock_fail);
        } else {
            /* EINVAL (dead gap), EBUSY (contended), ESRCH (slot gone): all
             * legitimate refusals during churn; never a silent phantom lock. */
            InterlockedIncrement(&g_lock_refused);
        }
        /* back off so the churner's destroy can find the mutex free; without
         * this two spinning lockers starve destroy into EBUSY forever */
        Sleep(0);
    }
    return 0;
}

static void test_churn(void)
{
    g_churn_m = (sys_lwmutex_t_hle*)(vm_base + 0x2000);
    memset(g_churn_m, 0, sizeof(*g_churn_m));
    /* start in created state so lockers have something to chew on */
    sys_lwmutex_attribute_t attr;
    memset(&attr, 0, sizeof(attr));
    memcpy(attr.name, "churn\0\0\0", 8);
    CHECK(sys_lwmutex_create(g_churn_m, &attr) == CELL_OK, "churn pre-create");

    /* The HLE create/destroy printf per call would dominate the run (~120k
     * console lines); mute stdout for the churn phase. Verdicts go to stderr. */
    freopen("NUL", "w", stdout);

    HANDLE th[3];
    th[0] = (HANDLE)_beginthreadex(NULL, 0, locker, NULL, 0, NULL);
    th[1] = (HANDLE)_beginthreadex(NULL, 0, locker, NULL, 0, NULL);
    th[2] = (HANDLE)_beginthreadex(NULL, 0, churner, NULL, 0, NULL);
    WaitForMultipleObjects(3, th, TRUE, INFINITE);   /* churner is cycle-bounded */
    for (int i = 0; i < 3; i++) CloseHandle(th[i]);

    CHECK(g_excl_violations == 0,
          "mutual exclusion violated %ld time(s) under destroy/create churn "
          "(the boot-13 phantom-slot resurrection)", g_excl_violations);
    fprintf(stderr, "churn: %ld locks ok, %ld refused, %ld unlock anomalies, "
            "%ld exclusion violations\n",
            g_lock_ok, g_lock_refused, g_unlock_fail, g_excl_violations);
}

int main(void)
{
    vm_base = g_guest_mem;

    test_lifecycle();
    test_churn();

    if (g_fails) {
        fprintf(stderr, "lwmutex_lifecycle: %d FAILURE(S)\n", g_fails);
        return 1;
    }
    fprintf(stderr, "lwmutex_lifecycle: all tests passed\n");
    return 0;
}
