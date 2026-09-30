/* Inlined packed arithmetic for the native operation. Preserve the established
 * xfloat contract: extended exponent fallback, flushed denormals, uncontracted
 * double intermediates and float truncation under the worker's MXCSR mode. */
#include <immintrin.h>
static __forceinline __m128i vector_bits(Vec a) {
    return _mm_loadu_si128((const __m128i*)&a);
}
static __forceinline __m128i flush(__m128i value) {
    const __m128i exp=_mm_and_si128(value,_mm_set1_epi32(0x7f800000));
    return _mm_and_si128(value,_mm_cmpgt_epi32(exp,_mm_setzero_si128()));
}
static __forceinline Vec greater(Vec a,Vec b) {
    __m128i x=flush(vector_bits(a)),y=flush(vector_bits(b));
    /* SPU values are ordered finite sign/magnitudes, including exponent 255. */
    x=_mm_xor_si128(x,_mm_and_si128(_mm_srai_epi32(x,31),_mm_set1_epi32(0x7fffffff)));
    y=_mm_xor_si128(y,_mm_and_si128(_mm_srai_epi32(y,31),_mm_set1_epi32(0x7fffffff)));
    Vec result;_mm_storeu_si128((__m128i*)&result,_mm_cmpgt_epi32(x,y));return result;
}
static __forceinline int magnitude_below(Vec a,Vec b,int limit) {
    const __m128i mask=_mm_set1_epi32(0x7fffffff);
    const __m128i x=_mm_and_si128(vector_bits(a),mask),y=_mm_and_si128(vector_bits(b),mask);
    return _mm_movemask_epi8(_mm_cmpgt_epi32(_mm_max_epi32(x,y),_mm_set1_epi32(limit-1)))==0;
}
static __forceinline Vec float_result(__m128 value) {
    Vec result;_mm_storeu_si128((__m128i*)&result,flush(_mm_castps_si128(value)));return result;
}
static __forceinline int ordinary(Vec a,Vec b) {
    const __m128i mask=_mm_set1_epi32(0x7f800000);
    const __m128i x=_mm_cmpeq_epi32(_mm_and_si128(vector_bits(a),mask),mask);
    const __m128i y=_mm_cmpeq_epi32(_mm_and_si128(vector_bits(b),mask),mask);
    return _mm_movemask_epi8(_mm_or_si128(x,y))==0;
}
static __forceinline __m256d decode(Vec a) {
    const __m128i value=vector_bits(a);
    const __m128i exp=_mm_and_si128(value,_mm_set1_epi32(0x7f800000));
    const __m128i normal=_mm_cmpgt_epi32(exp,_mm_setzero_si128());
    return _mm256_cvtps_pd(_mm_castsi128_ps(_mm_and_si128(value,normal)));
}
static __forceinline int encode(__m256d value,Vec* out) {
    const __m256i exp=_mm256_and_si256(_mm256_srli_epi64(_mm256_castpd_si256(value),52),_mm256_set1_epi64x(0x7ff));
    if(_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(exp,_mm256_set1_epi64x(0x47e)))))return 0;
    __m128i result=_mm_castps_si128(_mm256_cvtpd_ps(value));
    const __m128i exponent=_mm_and_si128(result,_mm_set1_epi32(0x7f800000));
    result=_mm_and_si128(result,_mm_cmpgt_epi32(exponent,_mm_setzero_si128()));
    _mm_storeu_si128((__m128i*)out,result);return 1;
}
static __forceinline Vec add(Vec a,Vec b) {
    /* Each operand <2^126: the sum cannot enter the extended range. */
    if(magnitude_below(a,b,0x7e800000))return float_result(_mm_add_ps(_mm_castsi128_ps(flush(vector_bits(a))),_mm_castsi128_ps(flush(vector_bits(b)))));
    Vec out;if(ordinary(a,b)&&encode(_mm256_add_pd(decode(a),decode(b)),&out))return out;
    return spu_fa(a,b);
}
static __forceinline Vec sub(Vec a,Vec b) {
    if(magnitude_below(a,b,0x7e800000))return float_result(_mm_sub_ps(_mm_castsi128_ps(flush(vector_bits(a))),_mm_castsi128_ps(flush(vector_bits(b)))));
    Vec out;if(ordinary(a,b)&&encode(_mm256_sub_pd(decode(a),decode(b)),&out))return out;
    return spu_fs(a,b);
}
static __forceinline Vec mul(Vec a,Vec b) {
    /* Each operand <2^63: float multiplication has exactly the same final
     * truncation as the reference's exact double product and encode. */
    if(magnitude_below(a,b,0x5f000000))return float_result(_mm_mul_ps(_mm_castsi128_ps(flush(vector_bits(a))),_mm_castsi128_ps(flush(vector_bits(b)))));
    Vec out;if(ordinary(a,b)&&encode(_mm256_mul_pd(decode(a),decode(b)),&out))return out;
    return spu_fm(a,b);
}
static __forceinline Vec madd(Vec a,Vec b,Vec c) {
    Vec out;
    if(ordinary(a,b)&&ordinary(c,c)) {
        const __m256d product=_mm256_mul_pd(decode(a),decode(b));
        if(encode(_mm256_add_pd(product,decode(c)),&out))return out;
    }
    return spu_fma(a,b,c);
}
