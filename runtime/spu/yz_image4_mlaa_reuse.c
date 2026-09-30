#include "ps3emu/yz_image4_mlaa_reuse.h"
#include "../../libs/video/rsx_image4_mlaa.h"
#include "../../libs/spurs/cellSpurs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <emmintrin.h>
extern uint8_t* vm_base;
static SRWLOCK lock=SRWLOCK_INIT;
static int decision;
static uint64_t task_round[5],round;
static unsigned seen[5];
static uint32_t record_ea[5];
static uint8_t records[5][64];

static void fault(const char* why) {
    /* Once a group is admitted, silently mixing an original worker into it
     * would corrupt the in-place operation. A contract/order violation is a
     * failed attempt, never permission to fall back halfway through. */
    fprintf(stderr,"[image4-mlaa-reuse] fatal contract/order violation: %s\n",why);fflush(stderr);abort();
}
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static int copy_guest(void* dst,uint32_t ea,size_t n) {
    if(!vm_base || !ea || (uint64_t)ea+n>UINT64_C(0x100000000))return 0;
    __try {memcpy(dst,vm_base+ea,n);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 1;
}
static int exact_black_image(uint32_t ea) {
    enum {PITCH=4096,HEIGHT=768};
    if(!vm_base || !ea || (uint64_t)ea+PITCH*(HEIGHT+1)>UINT64_C(0x100000000))return 0;
    __try {
        for(unsigned y=0;y<=HEIGHT;++y) {
            /* Guest ARGB is byte ordered: only the first byte of each pixel
             * is ten on the final four image rows. The extra row is all zero. */
            __m128i expected=_mm_set1_epi32(y>=764 && y<768?10:0);
            for(unsigned x=0;x<PITCH;x+=16) {
                __m128i v=_mm_loadu_si128((const __m128i*)(vm_base+ea+y*PITCH+x));
                if(_mm_movemask_epi8(_mm_cmpeq_epi8(v,expected))!=0xffff)return 0;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    return 1;
}
static int admit(const uint8_t* own,uint32_t own_ea) {
    uint32_t ids[5],eas[5];rsx_image4_mlaa_image image;
    if(!cellSpursSnapshotImage4Taskset(4,ids,eas,&image.fingerprint,&image.image_size,&image.entry_pc,&image.image_id) ||
       image.image_id!=4 || !rsx_image4_mlaa_image_matches(&image))return 0;
    uint8_t raw[5][64],verify[64];uint32_t mask=0;
    for(unsigned i=0;i<5;++i) {
        if(!copy_guest(raw[i],eas[i],64))return 0;
        unsigned id=raw[i][35]>>4;
        if(id>=5 || (mask&(1u<<id)))return 0;
        mask|=1u<<id;memcpy(records[id],raw[i],64);record_ea[id]=eas[i];
    }
    unsigned own_id=own[35]>>4;
    if(own_id>=5 || record_ea[own_id]!=own_ea || memcmp(own,records[own_id],64))return 0;
    rsx_image4_mlaa_contract contract;
    uint32_t image_ea=be32(records[0]+12),label_ea=be32(records[0]+4),value=be32(records[0]);
    if(rsx_image4_mlaa_validate(records,image_ea,label_ea,value,&contract)!=RSX_IMAGE4_MLAA_ACCEPT ||
       label_ea<0x10200000u || label_ea>=0x10210000u || (label_ea&3))return 0;
    uint8_t counter[4],direction[128],zero[128]={0};
    if(!copy_guest(counter,contract.counter_ea,4) || be32(counter)!=(uint32_t)(value-1) ||
       !copy_guest(direction,contract.direction_lock_ea,128) || memcmp(direction,zero,128) ||
       !exact_black_image(image_ea))return 0;
    /* The SPURS signal owns the image until the original final publication.
     * Verify all five immutable descriptors around the full image check. */
    MemoryBarrier();
    for(unsigned id=0;id<5;++id)
        if(!copy_guest(verify,record_ea[id],64) || memcmp(verify,records[id],64))return 0;
    return 1;
}
int yz_image4_mlaa_reuse(const uint8_t* ls,uint32_t lsa,uint32_t ea,unsigned phase) {
    if(!ls || lsa>262144-64 || phase>3)fault("invalid callsite");
    const uint8_t* own=ls+lsa;unsigned id=own[35]>>4;
    if(id>=5 || (own[35]&15)!=5)fault("unexpected task group");
    AcquireSRWLockExclusive(&lock);
    if(!phase) {
        uint64_t incoming=++task_round[id];
        if(incoming>round) {
            if(round)for(unsigned i=0;i<5;++i)if(seen[i]!=4)fault("next round before all prior phases");
            round=incoming;memset(seen,0,sizeof(seen));decision=admit(own,ea);
        } else if(incoming!=round)fault("task round skew");
    } else if(task_round[id]!=round)fault("phase belongs to another round");
    if(seen[id]!=phase)fault("duplicate or out-of-order phase");
    if(decision && (record_ea[id]!=ea || memcmp(own,records[id],64)))fault("admitted task changed");
    ++seen[id];int reuse=decision;
    ReleaseSRWLockExclusive(&lock);
    return reuse;
}
#else
int yz_image4_mlaa_reuse(const uint8_t* ls,uint32_t lsa,uint32_t ea,unsigned phase) {
    (void)ls;(void)lsa;(void)ea;(void)phase;return 0;
}
#endif
