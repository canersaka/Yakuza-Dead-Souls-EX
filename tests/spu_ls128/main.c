#define YZ_PERF_CLEAN 1

#include "spu_context.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

static int failures;
static volatile uint64_t benchmark_sink;

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); \
    ++failures; } } while (0)

void yz_tagread_repair_read(
    struct spu_context* ctx, uint32_t lsa, uint32_t* value)
{
    (void)ctx;
    (void)lsa;
    (void)value;
}

static uint32_t next_random(uint32_t* state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static u128 scalar_read(const uint8_t* p)
{
    u128 value;
    for (int i = 0; i < 4; ++i) {
        value._u32[i] = ((uint32_t)p[i * 4] << 24) |
                        ((uint32_t)p[i * 4 + 1] << 16) |
                        ((uint32_t)p[i * 4 + 2] << 8) |
                        (uint32_t)p[i * 4 + 3];
    }
    return value;
}

static void scalar_write(uint8_t* p, u128 value)
{
    for (int i = 0; i < 4; ++i) {
        const uint32_t word = value._u32[i];
        p[i * 4] = (uint8_t)(word >> 24);
        p[i * 4 + 1] = (uint8_t)(word >> 16);
        p[i * 4 + 2] = (uint8_t)(word >> 8);
        p[i * 4 + 3] = (uint8_t)word;
    }
}

static void randomized_equivalence(void)
{
    spu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    uint32_t random = 0x4c533132u;
    for (uint32_t iteration = 0; iteration < 100000u; ++iteration) {
        const uint32_t lsa = next_random(&random) & (SPU_LS_MASK & ~0xFu);
        for (uint32_t byte = 0; byte < 16u; ++byte)
            ctx.ls[lsa + byte] = (uint8_t)next_random(&random);
        const u128 expected = scalar_read(&ctx.ls[lsa]);
        const u128 actual = spu_ls_read128(&ctx, lsa);
        CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0);

        u128 written;
        for (uint32_t word = 0; word < 4u; ++word)
            written._u32[word] = next_random(&random);
        uint8_t expected_bytes[16];
        scalar_write(expected_bytes, written);
        spu_ls_write128(&ctx, lsa, written);
        CHECK(memcmp(&ctx.ls[lsa], expected_bytes, sizeof(expected_bytes)) == 0);
    }
}

static uint64_t cycle_clock(void)
{
#if defined(_MSC_VER) || defined(__i386__) || defined(__x86_64__)
    return (uint64_t)__rdtsc();
#else
    return 0u;
#endif
}

static void benchmark(void)
{
    spu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    for (uint32_t i = 0; i < sizeof(ctx.ls); ++i)
        ctx.ls[i] = (uint8_t)(i * 37u + 11u);

    u128 value = scalar_read(&ctx.ls[0x1000]);
    const uint32_t iterations = 10000000u;
    const uint64_t start = cycle_clock();
    for (uint32_t i = 0; i < iterations; ++i) {
        const uint32_t lsa = 0x1000u + ((i & 255u) << 4);
        value = spu_ls_read128(&ctx, lsa);
        value._u32[0] += i;
        spu_ls_write128(&ctx, lsa, value);
    }
    const uint64_t elapsed = cycle_clock() - start;
    benchmark_sink = value._u64[0];
    printf("spu ls128 mode=%s cycles=%llu operations=%u cycles_per_pair=%.3f\n",
           YZ_SPU_SIMD_LS128 ? "simd" : "scalar",
           (unsigned long long)elapsed, iterations,
           (double)elapsed / (double)iterations);
}

int main(void)
{
    randomized_equivalence();
    benchmark();
    if (failures) {
        fprintf(stderr, "spu ls128 tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("spu ls128 tests: PASS");
    return 0;
}
