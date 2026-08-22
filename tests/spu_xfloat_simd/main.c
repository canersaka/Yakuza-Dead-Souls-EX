#include "spu_helpers.h"
#include "spu_xfloat_simd.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

const yz_runtime_config g_yz_runtime_config = {0};

static uint64_t s_rng = UINT64_C(0xD1B54A32D192ED03);
static volatile uint64_t s_sink;

static uint32_t random_u32(void)
{
    uint64_t x = s_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_rng = x;
    return (uint32_t)((x * UINT64_C(2685821657736338717)) >> 32);
}

static uint32_t random_ordinary_xfloat(void)
{
    uint32_t bits = random_u32();
    if (((bits >> 23) & 0xFFu) == 0xFFu)
        bits ^= 1u << 23; /* exponent 0xFE, mantissa/sign unchanged */
    return bits;
}

static u128 random_vector(void)
{
    u128 value;
    int lane;
    for (lane = 0; lane < 4; ++lane)
        value._u32[lane] = random_ordinary_xfloat();
    return value;
}

static int equal128(u128 a, u128 b)
{
    return a._u64[0] == b._u64[0] && a._u64[1] == b._u64[1];
}

static void print_mismatch(const char* op, uint32_t iteration,
                           u128 actual, u128 expected)
{
    fprintf(stderr,
            "%s mismatch iteration=%u actual=%08X,%08X,%08X,%08X "
            "expected=%08X,%08X,%08X,%08X\n",
            op, iteration,
            actual._u32[0], actual._u32[1], actual._u32[2], actual._u32[3],
            expected._u32[0], expected._u32[1], expected._u32[2], expected._u32[3]);
}

#define CHECK_OP2(name, public_fn, scalar_fn) do {                         \
    u128 actual_ = public_fn(a, b);                                        \
    u128 expected_ = scalar_fn(a, b);                                      \
    if (!equal128(actual_, expected_)) {                                   \
        print_mismatch(name, iteration, actual_, expected_);               \
        return 1;                                                          \
    }                                                                      \
} while (0)

#define CHECK_OP3(name, public_fn, scalar_fn) do {                         \
    u128 actual_ = public_fn(a, b, c);                                     \
    u128 expected_ = scalar_fn(a, b, c);                                   \
    if (!equal128(actual_, expected_)) {                                   \
        print_mismatch(name, iteration, actual_, expected_);               \
        return 1;                                                          \
    }                                                                      \
} while (0)

static int differential_test(uint32_t count)
{
    uint32_t iteration;
    for (iteration = 0; iteration < count; ++iteration) {
        u128 a = random_vector();
        u128 b = random_vector();
        u128 c = random_vector();
        CHECK_OP2("fa", spu_fa, spu_xf_scalar_fa);
        CHECK_OP2("fs", spu_fs, spu_xf_scalar_fs);
        CHECK_OP2("fm", spu_fm, spu_xf_scalar_fm);
        CHECK_OP3("fma", spu_fma, spu_xf_scalar_fma);
        CHECK_OP3("fms", spu_fms, spu_xf_scalar_fms);
        CHECK_OP3("fnms", spu_fnms, spu_xf_scalar_fnms);
    }
    return 0;
}

static int boundary_test(void)
{
    static const uint32_t values[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu,
        0x00800000u, 0x80800000u, 0x00800001u, 0x3F000000u,
        0x3F7FFFFFu, 0x3F800000u, 0x3F800001u, 0x40000000u,
        0x7EFFFFFFu, 0x7F000000u, 0x7F7FFFFFu, 0xFEFFFFFFu,
        0xFF000000u, 0xFFFFFFFFu, 0x7F800000u, 0xFF800001u,
    };
    uint32_t iteration = 0;
    size_t i;
    size_t j;
    size_t k;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        for (j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            u128 a;
            u128 b;
            u128 c;
            for (k = 0; k < 4; ++k) {
                a._u32[k] = values[(i + k) % (sizeof(values) / sizeof(values[0]))];
                b._u32[k] = values[(j + k * 3) % (sizeof(values) / sizeof(values[0]))];
                c._u32[k] = values[(i + j + k * 5) % (sizeof(values) / sizeof(values[0]))];
            }
            CHECK_OP2("boundary fa", spu_fa, spu_xf_scalar_fa);
            CHECK_OP2("boundary fs", spu_fs, spu_xf_scalar_fs);
            CHECK_OP2("boundary fm", spu_fm, spu_xf_scalar_fm);
            CHECK_OP3("boundary fma", spu_fma, spu_xf_scalar_fma);
            CHECK_OP3("boundary fms", spu_fms, spu_xf_scalar_fms);
            CHECK_OP3("boundary fnms", spu_fnms, spu_xf_scalar_fnms);
            ++iteration;
        }
    }
    return 0;
}

static int fallback_test(void)
{
    u128 a = spu_splat_u32(0x3F800000u);
    u128 b = spu_splat_u32(0x40000000u);
    u128 c = spu_splat_u32(0x3F000000u);
    u128 out;
    a._u32[2] = 0x7F800001u;
    if (spu_xfloat_simd_fa(&out, &a, &b) != 0 ||
        spu_xfloat_simd_fma(&out, &a, &b, &c) != 0) {
        fputs("extended input did not request scalar fallback\n", stderr);
        return 1;
    }
    a = spu_splat_u32(0x7F7FFFFFu);
    b = spu_splat_u32(0x40000000u);
    if (spu_xfloat_simd_fm(&out, &a, &b) != 0) {
        fputs("extended result did not request scalar fallback\n", stderr);
        return 1;
    }
    return 0;
}

static void benchmark(uint32_t count)
{
    u128 a = {{0}};
    u128 b = {{0}};
    u128 c = {{0}};
    u128 r;
    uint32_t i;
    uint64_t simd_begin;
    uint64_t simd_end;
    uint64_t scalar_begin;
    uint64_t scalar_end;
    a._u32[0] = 0x3F123456u; a._u32[1] = 0xBF654321u;
    a._u32[2] = 0x40123456u; a._u32[3] = 0x3E654321u;
    b._u32[0] = 0x40012345u; b._u32[1] = 0x3E712345u;
    b._u32[2] = 0xBF012345u; b._u32[3] = 0x3F712345u;
    c._u32[0] = 0x3E123456u; c._u32[1] = 0xBE654321u;
    c._u32[2] = 0x3F345678u; c._u32[3] = 0xBF234567u;
#ifdef _MSC_VER
    simd_begin = __rdtsc();
#else
    simd_begin = 0;
#endif
    r = c;
    for (i = 0; i < count; ++i) {
        r = spu_fma(a, b, r);
        r = spu_fm(r, c);
    }
#ifdef _MSC_VER
    simd_end = __rdtsc();
#else
    simd_end = simd_begin;
#endif
    s_sink ^= r._u64[0] ^ r._u64[1];
#ifdef _MSC_VER
    scalar_begin = __rdtsc();
#else
    scalar_begin = 0;
#endif
    r = c;
    for (i = 0; i < count; ++i) {
        r = spu_xf_scalar_fma(a, b, r);
        r = spu_xf_scalar_fm(r, c);
    }
#ifdef _MSC_VER
    scalar_end = __rdtsc();
#else
    scalar_end = scalar_begin;
#endif
    s_sink ^= r._u64[0] ^ r._u64[1];
    printf("spu xfloat benchmark helper_calls=%u simd_cycles=%" PRIu64
           " simd_cycles_per_call=%.3f scalar_cycles=%" PRIu64
           " scalar_cycles_per_call=%.3f speedup=%.2fx\n",
           count * 2u,
           simd_end - simd_begin,
           (double)(simd_end - simd_begin) / (double)(count * 2u),
           scalar_end - scalar_begin,
           (double)(scalar_end - scalar_begin) / (double)(count * 2u),
           (double)(scalar_end - scalar_begin) / (double)(simd_end - simd_begin));
}

int main(void)
{
    if (!spu_xfloat_simd_thread_enter()) {
        puts("spu xfloat simd tests: SKIP (AVX2 unavailable)");
        return 0;
    }
    if (fallback_test() || boundary_test() || differential_test(1000000u))
        return 1;
    benchmark(1000000u);
    puts("spu xfloat simd tests: PASS (6,000,000 randomized operation vectors)");
    return 0;
}
