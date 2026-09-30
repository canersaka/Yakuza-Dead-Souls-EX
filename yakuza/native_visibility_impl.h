/* Template header with no include guard: it is included once per entry point,
 * with YZ_VISIBILITY_ENTRY defined by the includer. */
#include "native_visibility.h"
#include "spu_helpers.h"
#include <string.h>

typedef u128 Vec;
static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static Vec load(const uint8_t* p) {
    Vec v; for (unsigned i=0;i<4;++i) v._u32[i]=read_be32(p+4*i); return v;
}
static Vec bits(uint32_t x) { Vec v; for(unsigned i=0;i<4;++i)v._u32[i]=x; return v; }
static Vec lane(Vec v, unsigned i) { return bits(v._u32[i]); }
#if defined(YZ_NATIVE_VISIBILITY_AVX2)
#include "native_visibility_math_avx2.h"
#else
static Vec add(Vec a,Vec b) { return spu_fa(a,b); }
static Vec sub(Vec a,Vec b) { return spu_fs(a,b); }
static Vec mul(Vec a,Vec b) { return spu_fm(a,b); }
static Vec madd(Vec a,Vec b,Vec c) { return spu_fma(a,b,c); }
static Vec greater(Vec a,Vec b) { return spu_fcgt(a,b); }
#endif
static Vec absolute(Vec a) { for(unsigned i=0;i<4;++i)a._u32[i]&=0x7fffffffu; return a; }
static int all_greater(Vec a,Vec b) {
    const Vec c=greater(a,b);
    return (c._u32[0]&c._u32[1]&c._u32[2]&c._u32[3]) != 0;
}
static Vec maximum(Vec a,Vec b) { return spu_selb(b,a,greater(a,b)); }
static Vec project(Vec p,const Vec m[3]) {
    return madd(lane(p,2),m[2],madd(lane(p,1),m[1],mul(lane(p,0),m[0])));
}
static Vec dot3(Vec a, Vec b) {
    Vec p=mul(a,b); return add(add(lane(p,0),lane(p,1)),lane(p,2));
}
static Vec transform_direction(Vec p,const Vec m[4]) {
    /* Preserve the game's operation grouping, including truncation. */
    return madd(lane(p,2),m[2],madd(lane(p,0),m[0],mul(lane(p,1),m[1])));
}
static void quaternion_columns(Vec q,Vec out[3]) {
    Vec square=mul(q,q), doubled=add(square,square);
    Vec rotate={{0}};
    rotate._u32[0]=q._u32[1]; rotate._u32[1]=q._u32[2];
    rotate._u32[2]=q._u32[0]; rotate._u32[3]=q._u32[1];
    Vec cross=mul(q,rotate); cross=add(cross,cross);
    Vec qw=mul(q,lane(q,3)); qw=add(qw,qw);
    Vec mixed; mixed._u32[0]=qw._u32[2]; mixed._u32[1]=qw._u32[0];
    mixed._u32[2]=qw._u32[1]; mixed._u32[3]=qw._u32[2];
    Vec plus=add(cross,mixed), minus=sub(cross,mixed);
    Vec rotated_square;
    rotated_square._u32[0]=doubled._u32[1]; rotated_square._u32[1]=doubled._u32[2];
    rotated_square._u32[2]=doubled._u32[0]; rotated_square._u32[3]=doubled._u32[1];
    Vec diag=sub(bits(0x3f800000u),add(doubled,rotated_square));
    out[0]=bits(0); out[1]=bits(0); out[2]=bits(0);
    out[0]._u32[0]=diag._u32[1]; out[0]._u32[1]=plus._u32[0]; out[0]._u32[2]=minus._u32[2];
    out[1]._u32[0]=minus._u32[0]; out[1]._u32[1]=diag._u32[2]; out[1]._u32[2]=plus._u32[1];
    out[2]._u32[0]=plus._u32[2]; out[2]._u32[1]=minus._u32[1]; out[2]._u32[2]=diag._u32[0];
}
typedef struct Camera {
    Vec near_limit, size_limit, size_scale, occluder_margin, side[3];
    uint32_t occluders;
    const uint8_t* bytes;
} Camera;
static int in_frustum(const Camera* c,Vec center,const Vec axis[3],Vec far_limit) {
    Vec radius=add(add(absolute(lane(axis[0],2)),absolute(lane(axis[1],2))),absolute(lane(axis[2],2)));
    if(!all_greater(add(lane(center,2),radius),far_limit))return 0;
    if(!all_greater(c->near_limit,sub(lane(center,2),radius)))return 0;
    Vec a=absolute(project(axis[0],c->side));
    Vec b=absolute(project(axis[1],c->side));
    Vec d=absolute(project(axis[2],c->side));
    /* The original sums the second and third axes first. */
    return all_greater(add(add(b,d),a),project(center,c->side));
}
static uint32_t select_bits(uint32_t a,uint32_t b,uint32_t mask) {
    return (a&~mask)|(b&mask);
}
static int not_occluded(const Camera* c,Vec center,const Vec axis[3]) {
    Vec largest=maximum(maximum(absolute(axis[0]),absolute(axis[1])),absolute(axis[2]));
    Vec high=add(center,largest), low=sub(center,largest), zero=bits(0);
    const uint32_t positive=all_greater(lane(high,0),zero)|(all_greater(lane(high,1),zero)<<1);
    const uint32_t negative=all_greater(zero,lane(low,0))|(all_greater(zero,lane(low,1))<<1);
    const uint32_t sectors=select_bits(negative&(positive*2),negative,negative>>1)|
        select_bits((positive<<2)&(positive<<3),negative<<2,positive*2);
    const Vec quantized=spu_cflts(low,163);
    for(uint32_t i=0;i<c->occluders;++i) {
        const uint8_t* key=c->bytes+0x500+8*i;
        if(quantized._s32[2]>(int32_t)read_be32(key))break;
        if((read_be32(key+4)&sectors)!=sectors)continue;
        Vec plane=load(c->bytes+0x900+16*i);
        Vec product=mul(center,plane);
        Vec distance=add(add(lane(product,0),lane(product,1)),add(lane(product,2),lane(plane,3)));
        if(!all_greater(c->occluder_margin,distance))continue;
        const uint8_t* p=c->bytes+0x1100+64*i;
        Vec edge[4]={load(p),load(p+16),load(p+32),load(p+48)};
        Vec inside=madd(edge[0],lane(center,0),madd(edge[1],lane(center,1),madd(edge[2],lane(center,2),edge[3])));
        if(!all_greater(inside,c->occluder_margin))continue;
        Vec radius=sub(sub(sub(zero,absolute(dot3(axis[0],plane))),absolute(dot3(axis[1],plane))),absolute(dot3(axis[2],plane)));
        if(!all_greater(radius,distance))continue;
        Vec extent[3];
        for(unsigned a=0;a<3;++a)
            extent[a]=absolute(madd(edge[0],lane(axis[a],0),madd(edge[1],lane(axis[a],1),madd(edge[2],lane(axis[a],2),edge[3]))));
        if(all_greater(inside,add(add(extent[0],extent[1]),extent[2])))return 0;
    }
    return 1;
}

int YZ_VISIBILITY_ENTRY(const yz_visibility_request* r,yz_visibility_result* out) {
    if(!r||!out||!r->object_matrix||!r->bounds||!r->camera||r->count>128||r->camera_bytes<0x1100)return 0;
    const uint32_t count=read_be32(r->camera);
    if(count>128||r->camera_bytes<0x1100+(size_t)count*64)return 0;
    Camera camera={load(r->camera+0x80),load(r->camera+0x40),load(r->camera+0x50),load(r->camera+0x60),
        {load(r->camera+0x90),load(r->camera+0xa0),load(r->camera+0xb0)},count,r->camera};
    Vec view[4],matrix[4];
    for(unsigned i=0;i<4;++i)view[i]=load(r->camera+0xc0+16*i);
    for(unsigned i=0;i<4;++i) {
        Vec v=load(r->object_matrix+16*i);
        Vec x=i==3?madd(lane(v,0),view[0],view[3]):mul(lane(v,0),view[0]);
        matrix[i]=madd(lane(v,2),view[2],madd(lane(v,1),view[1],x));
    }
    memset(out,0,sizeof(*out));
    for(uint32_t i=0;i<r->count;++i) {
        const uint8_t* bound=r->bounds+64*i;
        Vec source_center=load(bound+16),extent=load(bound+32),rotation[3],axis[3];
        const Vec quaternion=load(bound+48);
#if defined(YZ_NATIVE_VISIBILITY_AVX2)
        const int axis_aligned=quaternion._u32[0]==0 && quaternion._u32[1]==0 &&
            quaternion._u32[2]==0 && quaternion._u32[3]==0x3f800000u;
#else
        const int axis_aligned=0;
#endif
        if(!axis_aligned)quaternion_columns(quaternion,rotation);
        Vec center=add(madd(lane(source_center,0),matrix[0],mul(lane(source_center,1),matrix[1])),
                       madd(lane(source_center,2),matrix[2],matrix[3]));
        for(unsigned a=0;a<3;++a)
            axis[a]=axis_aligned?mul(lane(extent,a),matrix[a]):
                transform_direction(mul(lane(extent,a),rotation[a]),matrix);
        if(!in_frustum(&camera,center,axis,bits(r->far_limit_bits))||!not_occluded(&camera,center,axis))continue;
        Vec radius=add(add(absolute(axis[0]),absolute(axis[1])),absolute(axis[2]));
        Vec depth=lane(add(center,radius),2);
        if(!all_greater(depth,camera.near_limit)) {
            Vec reciprocal=spu_fi(depth,spu_frest(depth));
            reciprocal=spu_fnms(mul(reciprocal,reciprocal),depth,add(reciprocal,reciprocal));
            Vec size=mul(mul(reciprocal,lane(source_center,3)),camera.size_scale);
            if(all_greater(camera.size_limit,size))continue;
        }
        out->visible[i]=1;
        out->depth_bits[i]=depth._u32[0];
    }
    return 1;
}
