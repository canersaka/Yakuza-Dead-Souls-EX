#define YZ_VISIBILITY_ENTRY yz_visibility_oriented_reference
#include "native_visibility_impl.h"
#include "spu_xfloat_simd.h"

#if defined(YZ_NATIVE_VISIBILITY_AVX2_AVAILABLE)
int yz_visibility_oriented_avx2(const yz_visibility_request*,yz_visibility_result*);
#endif

int yz_visibility_oriented(const yz_visibility_request* request,yz_visibility_result* output) {
#if defined(YZ_NATIVE_VISIBILITY_AVX2_AVAILABLE)
    if(spu_xfloat_simd_thread_enter())return yz_visibility_oriented_avx2(request,output);
#endif
    return yz_visibility_oriented_reference(request,output);
}
