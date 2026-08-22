#include "spu_xfloat_simd.h"

#include <stdint.h>

#if defined(YZ_SPU_SIMD_XFLOAT) && YZ_SPU_SIMD_XFLOAT && \
    defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#include <windows.h>

volatile int g_spu_xfloat_simd_enabled = 0;

static INIT_ONCE s_xfloat_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK spu_xfloat_simd_init(PINIT_ONCE once, PVOID parameter,
                                          PVOID* context)
{
    int cpu[4] = {0, 0, 0, 0};
    (void)once;
    (void)parameter;
    (void)context;
    __cpuidex(cpu, 1, 0);
    if ((cpu[2] & (1 << 27)) == 0 || (cpu[2] & (1 << 28)) == 0)
        return TRUE;
    if ((_xgetbv(0) & 0x6) != 0x6)
        return TRUE;
    __cpuidex(cpu, 7, 0);
    if ((cpu[1] & (1 << 5)) != 0)
        g_spu_xfloat_simd_enabled = 1;
    return TRUE;
}

int spu_xfloat_simd_thread_enter(void)
{
    InitOnceExecuteOnce(&s_xfloat_init_once, spu_xfloat_simd_init, NULL, NULL);
    /* SPU xfloat arithmetic truncates.  MXCSR is thread-local, so establish
     * it at each lifted workload entry even after the process-wide CPU check. */
    if (g_spu_xfloat_simd_enabled) {
        unsigned csr = _mm_getcsr();
        csr = (csr & ~_MM_ROUND_MASK) | _MM_ROUND_TOWARD_ZERO;
        _mm_setcsr(csr);
    }
    return g_spu_xfloat_simd_enabled != 0;
}

static int spu_xfloat_inputs_ordinary(const u128* a, const u128* b,
                                      const u128* c)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (((a->_u32[i] >> 23) & 0xFFu) == 0xFFu ||
            ((b->_u32[i] >> 23) & 0xFFu) == 0xFFu ||
            (c && ((c->_u32[i] >> 23) & 0xFFu) == 0xFFu))
            return 0;
    }
    return 1;
}

static __m256d spu_xfloat_decode4(const u128* value)
{
    const __m128i bits = _mm_loadu_si128((const __m128i*)value->_u32);
    const __m128i exponent = _mm_and_si128(_mm_srli_epi32(bits, 23),
                                           _mm_set1_epi32(0xFF));
    const __m128i normal = _mm_cmpgt_epi32(exponent, _mm_setzero_si128());
    const __m128 flushed = _mm_castsi128_ps(_mm_and_si128(bits, normal));
    return _mm256_cvtps_pd(flushed);
}

static int spu_xfloat_encode4(u128* out, __m256d value)
{
    const __m256i abs_mask = _mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL);
    const __m256i abs_bits = _mm256_and_si256(_mm256_castpd_si256(value), abs_mask);
    const __m256i exponent = _mm256_and_si256(
        _mm256_srli_epi64(abs_bits, 52), _mm256_set1_epi64x(0x7FF));
    const __m256i extended = _mm256_cmpgt_epi64(
        exponent, _mm256_set1_epi64x(0x47E)); /* >= 2^128 => biased exp >= 0x47F */
    if (_mm256_movemask_pd(_mm256_castsi256_pd(extended)) != 0)
        return 0;

    {
        __m128 encoded = _mm256_cvtpd_ps(value);
        __m128i bits = _mm_castps_si128(encoded);
        const __m128i exp = _mm_and_si128(_mm_srli_epi32(bits, 23),
                                          _mm_set1_epi32(0xFF));
        const __m128i normal = _mm_cmpgt_epi32(exp, _mm_setzero_si128());
        bits = _mm_and_si128(bits, normal); /* result zero/denormal -> unsigned +0 */
        _mm_storeu_si128((__m128i*)out->_u32, bits);
    }
    return 1;
}

typedef enum spu_xfloat_binary_op {
    SPU_XF_ADD,
    SPU_XF_SUB,
    SPU_XF_MUL
} spu_xfloat_binary_op;

static int spu_xfloat_simd_binary(u128* out, const u128* a, const u128* b,
                                  spu_xfloat_binary_op op)
{
    __m256d av;
    __m256d bv;
    __m256d result;
    if (!g_spu_xfloat_simd_enabled || !spu_xfloat_inputs_ordinary(a, b, NULL))
        return 0;
    av = spu_xfloat_decode4(a);
    bv = spu_xfloat_decode4(b);
    if (op == SPU_XF_ADD)
        result = _mm256_add_pd(av, bv);
    else if (op == SPU_XF_SUB)
        result = _mm256_sub_pd(av, bv);
    else
        result = _mm256_mul_pd(av, bv);
    return spu_xfloat_encode4(out, result);
}

int spu_xfloat_simd_fa(u128* out, const u128* a, const u128* b)
{
    return spu_xfloat_simd_binary(out, a, b, SPU_XF_ADD);
}

int spu_xfloat_simd_fs(u128* out, const u128* a, const u128* b)
{
    return spu_xfloat_simd_binary(out, a, b, SPU_XF_SUB);
}

int spu_xfloat_simd_fm(u128* out, const u128* a, const u128* b)
{
    return spu_xfloat_simd_binary(out, a, b, SPU_XF_MUL);
}

typedef enum spu_xfloat_ternary_op {
    SPU_XF_MADD,
    SPU_XF_MSUB,
    SPU_XF_NMSUB
} spu_xfloat_ternary_op;

static int spu_xfloat_simd_ternary(u128* out, const u128* a, const u128* b,
                                   const u128* c, spu_xfloat_ternary_op op)
{
    __m256d product;
    __m256d cv;
    __m256d result;
    if (!g_spu_xfloat_simd_enabled || !spu_xfloat_inputs_ordinary(a, b, c))
        return 0;
    product = _mm256_mul_pd(spu_xfloat_decode4(a), spu_xfloat_decode4(b));
    cv = spu_xfloat_decode4(c);
    if (op == SPU_XF_MADD)
        result = _mm256_add_pd(product, cv);
    else if (op == SPU_XF_MSUB)
        result = _mm256_sub_pd(product, cv);
    else
        result = _mm256_sub_pd(cv, product);
    return spu_xfloat_encode4(out, result);
}

int spu_xfloat_simd_fma(u128* out, const u128* a, const u128* b, const u128* c)
{
    return spu_xfloat_simd_ternary(out, a, b, c, SPU_XF_MADD);
}

int spu_xfloat_simd_fms(u128* out, const u128* a, const u128* b, const u128* c)
{
    return spu_xfloat_simd_ternary(out, a, b, c, SPU_XF_MSUB);
}

int spu_xfloat_simd_fnms(u128* out, const u128* a, const u128* b, const u128* c)
{
    return spu_xfloat_simd_ternary(out, a, b, c, SPU_XF_NMSUB);
}

#else

volatile int g_spu_xfloat_simd_enabled = 0;
int spu_xfloat_simd_thread_enter(void) { return 0; }
#define SPU_XFLOAT_STUB2(name) \
    int name(u128* out, const u128* a, const u128* b) \
    { (void)out; (void)a; (void)b; return 0; }
#define SPU_XFLOAT_STUB3(name) \
    int name(u128* out, const u128* a, const u128* b, const u128* c) \
    { (void)out; (void)a; (void)b; (void)c; return 0; }
SPU_XFLOAT_STUB2(spu_xfloat_simd_fa)
SPU_XFLOAT_STUB2(spu_xfloat_simd_fs)
SPU_XFLOAT_STUB2(spu_xfloat_simd_fm)
SPU_XFLOAT_STUB3(spu_xfloat_simd_fma)
SPU_XFLOAT_STUB3(spu_xfloat_simd_fms)
SPU_XFLOAT_STUB3(spu_xfloat_simd_fnms)

#endif
