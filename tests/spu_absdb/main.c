#include "spu_helpers.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);
static volatile uint64_t benchmark_sink;

static uint64_t next_random(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * UINT64_C(0x2545f4914f6cdd1d);
}

static u128 reference_absdb(u128 a, u128 b)
{
    u128 result;
    for (unsigned lane = 0; lane < 16u; ++lane) {
        const uint8_t x = a._u8[lane];
        const uint8_t y = b._u8[lane];
        result._u8[lane] = (uint8_t)(x > y ? x - y : y - x);
    }
    return result;
}

static int check_pair(u128 a, u128 b, uint64_t ordinal)
{
    const u128 expected = reference_absdb(a, b);
    const u128 actual = spu_absdb(a, b);
    if (memcmp(&expected, &actual, sizeof(actual)) == 0)
        return 0;
    fprintf(stderr, "ABSDB mismatch at vector %" PRIu64 "\n", ordinal);
    return 1;
}

static int run_exactness(void)
{
    static const uint8_t boundaries[][16] = {
        { 0 },
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff },
        { 0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 0x55, 0xaa,
          0x10, 0x20, 0x40, 0x81, 0xc0, 0xe0, 0xf0, 0xf8 },
        { 0xff, 0xfe, 0x80, 0x7f, 0x01, 0x00, 0xaa, 0x55,
          0xf8, 0xf0, 0xe0, 0xc0, 0x81, 0x40, 0x20, 0x10 }
    };
    uint64_t ordinal = 0u;
    for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        for (unsigned j = 0; j < sizeof(boundaries) / sizeof(boundaries[0]); ++j) {
            u128 a;
            u128 b;
            memcpy(a._u8, boundaries[i], sizeof(a._u8));
            memcpy(b._u8, boundaries[j], sizeof(b._u8));
            if (check_pair(a, b, ordinal++))
                return 1;
        }
    }
    for (uint64_t i = 0; i < UINT64_C(1000000); ++i) {
        u128 a;
        u128 b;
        a._u64[0] = next_random();
        a._u64[1] = next_random();
        b._u64[0] = next_random();
        b._u64[1] = next_random();
        if (check_pair(a, b, ordinal++))
            return 1;
    }
    printf("ABSDB exactness: %" PRIu64 " vectors PASS\n", ordinal);
    return 0;
}

static double run_benchmark(void)
{
    enum { kInputs = 256, kIterations = 4000000 };
    u128 inputs[kInputs];
    u128 accumulator;
    for (unsigned i = 0; i < kInputs; ++i) {
        inputs[i]._u64[0] = next_random();
        inputs[i]._u64[1] = next_random();
    }
    accumulator._u64[0] = next_random();
    accumulator._u64[1] = next_random();

    const uint64_t start = __rdtsc();
    for (unsigned i = 0; i < kIterations; ++i)
        accumulator = spu_absdb(accumulator, inputs[i & (kInputs - 1)]);
    const uint64_t elapsed = __rdtsc() - start;
    benchmark_sink = accumulator._u64[0] ^ accumulator._u64[1];
    return (double)elapsed / (double)kIterations;
}

int main(void)
{
    if (run_exactness())
        return 1;
    const double cycles = run_benchmark();
    printf("ABSDB mode=%s cycles_per_helper=%.3f sink=%" PRIu64 "\n",
           YZ_SPU_SIMD_ABSDB ? "simd" : "scalar", cycles,
           benchmark_sink);
    return 0;
}
