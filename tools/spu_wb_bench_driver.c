/* WB microbenchmark driver: times spu_diff_run() over the s50 deterministic
 * environment for one linked twin (DIAG, FAST, or WB -- selected by which
 * generated TU(s) the build links, exactly like the diff harness).
 *
 * Usage: <exe> code.bin base entry_hex seed evbudget hopmax reps
 * Output: one line,
 *   BENCH entry=%X term=%s status=%u hops=%llu ev=%u reps=%u
 *         total_us=%llu avg_us=%.1f
 * Context re-init happens OUTSIDE the timed window; the timed region is
 * exactly the architectural execution (dispatch loop + lifted code + stub
 * environment, identical for every twin). */
#define SPU_DIFF_NO_MAIN
#include "spu_diff_harness.c"

#if defined(_WIN32)
typedef union { long long QuadPart; } yz_li_t;
__declspec(dllimport) int __stdcall QueryPerformanceCounter(yz_li_t*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(yz_li_t*);
static long long qpc(void) { yz_li_t t; QueryPerformanceCounter(&t); return t.QuadPart; }
static long long qpf(void) { yz_li_t t; QueryPerformanceFrequency(&t); return t.QuadPart; }
#else
#include <time.h>
static long long qpc(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec; }
static long long qpf(void) { return 1000000000LL; }
#endif

static spu_context g_bctx;

int main(int argc, char** argv)
{
    if (argc != 8) {
        fprintf(stderr, "usage: %s code.bin base entry seed evbudget hopmax reps\n",
                argv[0]);
        return 2;
    }
    uint32_t base = (uint32_t)strtoul(argv[2], 0, 16);
    uint32_t entry = (uint32_t)strtoul(argv[3], 0, 16);
    uint64_t seed = strtoull(argv[4], 0, 0);
    g_ev_budget = (uint32_t)strtoul(argv[5], 0, 0);
    uint64_t hopmax = strtoull(argv[6], 0, 0);
    unsigned reps = (unsigned)strtoul(argv[7], 0, 0);
    if (spu_diff_load_code(argv[1], base) != 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    REGISTER_FN();
    spu_diff_env_reset(seed);

    /* warm-up (also captures the reference terminal facts) */
    spu_diff_ctx_init(&g_bctx);
    const char* term = spu_diff_run(&g_bctx, entry, hopmax);
    uint64_t hops = spu_diff_hops();
    uint32_t nev = spu_diff_nev();
    uint32_t status = g_bctx.status;

    long long total = 0;
    for (unsigned r = 0; r < reps; r++) {
        /* fresh architectural state per rep, outside the timed window; the
         * event log is reset so the run replays the identical env stream */
        g_nev = 0;
        memset(g_ch_idx, 0, sizeof g_ch_idx);
        g_dec = 0x40000000ull;
        g_last_atomic = 0;
        g_sentinel_hit = 0;
        g_hops_total = 0;
        g_spu_trampoline_fn = 0;
        spu_diff_ctx_init(&g_bctx);
        long long t0 = qpc();
        const char* t = spu_diff_run(&g_bctx, entry, hopmax);
        long long t1 = qpc();
        total += (t1 - t0);
        if (t != term) {
            fprintf(stderr, "NONDETERMINISTIC: rep %u term %s vs %s\n", r, t, term);
            return 3;
        }
    }
    double us = (double)total * 1e6 / (double)qpf();
    printf("BENCH entry=%X term=%s status=%u hops=%llu ev=%u reps=%u "
           "total_us=%.0f avg_us=%.2f\n",
           entry, term, status, (unsigned long long)hops, nev, reps,
           us, reps ? us / reps : 0.0);
    return 0;
}
