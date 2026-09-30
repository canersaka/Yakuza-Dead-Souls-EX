#include "native_pose_generate.h"
#include "spu_helpers.h"
#include <emmintrin.h>
/* Reuse only the arithmetic value definitions. No architectural context,
 * registers, local store, channels or instruction dispatch participate. */
static uint32_t be32(const uint8_t* p)
{ return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static void store32(uint8_t* p,uint32_t v)
{ p[0]=(uint8_t)(v>>24);p[1]=(uint8_t)(v>>16);p[2]=(uint8_t)(v>>8);p[3]=(uint8_t)v; }
static int disjoint(const void* output,size_t bytes,const void* input,size_t length)
{
    const uintptr_t a=(uintptr_t)output,b=(uintptr_t)input;
    return output && input && bytes<=UINTPTR_MAX-a && length<=UINTPTR_MAX-b &&
        (a+bytes<=b || b+length<=a);
}
static u128 splat(uint32_t value)
{ u128 v;for(unsigned i=0;i<4;++i) v._u32[i]=value;return v; }
static void compose(u128 result[4],const u128 source[4],const uint8_t* coefficients)
{
    for(unsigned row=0;row<4;++row) {
        const u128 a=splat(be32(coefficients+row*16));
        u128 value=row==3?spu_xf_scalar_fma(a,source[0],source[3]):spu_xf_scalar_fm(a,source[0]);
        value=spu_xf_scalar_fma(splat(be32(coefficients+row*16+4)),source[1],value);
        result[row]=spu_xf_scalar_fma(splat(be32(coefficients+row*16+8)),source[2],value);
    }
}
int yz_pose_generate_spu48(void* output,size_t output_bytes,
    const void* world64,size_t world_bytes,const void* bones64,size_t bone_bytes,
    const void* model64,size_t model_bytes,uint32_t count)
{
    if(count>65535) return 0;
    if(!count) return 1;
    const size_t input=(size_t)count*64,bytes=(size_t)count*48;
    if(output_bytes<bytes || world_bytes<64 || bone_bytes<input || model_bytes<input ||
       !disjoint(output,bytes,world64,64) || !disjoint(output,bytes,bones64,input) ||
       !disjoint(output,bytes,model64,input)) return 0;
    const unsigned csr=_mm_getcsr();
    _mm_setcsr((csr&~_MM_ROUND_MASK)|_MM_ROUND_TOWARD_ZERO);
    u128 world[4];for(unsigned row=0;row<4;++row) for(unsigned lane=0;lane<4;++lane)
        world[row]._u32[lane]=be32((const uint8_t*)world64+row*16+lane*4);
    for(uint32_t bone=0;bone<count;++bone) {
        u128 first[4],final[4];compose(first,world,(const uint8_t*)bones64+(size_t)bone*64);
        compose(final,first,(const uint8_t*)model64+(size_t)bone*64);
        for(unsigned row=0;row<3;++row) for(unsigned lane=0;lane<4;++lane)
            store32((uint8_t*)output+(size_t)bone*48+row*16+lane*4,final[lane]._u32[row]);
    }
    _mm_setcsr(csr);return 1;
}
